#include "model.hpp"

#include "bits.hpp"

namespace cutoff {

namespace {

constexpr std::int32_t as_signed(std::uint32_t v) { return static_cast<std::int32_t>(v); }

constexpr bool word_in_ram(std::uint32_t word_addr) { return static_cast<std::uint64_t>(word_addr) + 3 < kRamBytes; }

}  // namespace

Model::Model(ModelConfig cfg) : cfg_(cfg), ram_(kRamBytes, 0) {}

void Model::load(std::span<const std::uint32_t> code) {
  for (std::size_t i = 0; i < code.size() && 4 * i + 3 < ram_.size(); ++i) write_word(static_cast<std::uint32_t>(4 * i), code[i]);
}

void Model::write_word(std::uint32_t addr, std::uint32_t value) {
  for (unsigned b = 0; b < 4; ++b) ram_.at(addr + b) = static_cast<std::uint8_t>(value >> (8 * b));
}

std::uint32_t Model::read_word(std::uint32_t addr) const {
  std::uint32_t v = 0;
  for (unsigned b = 0; b < 4; ++b) v |= static_cast<std::uint32_t>(ram_.at(addr + b)) << (8 * b);
  return v;
}

void Model::set_reg(unsigned r, std::uint32_t value) {
  if (r != 0) regs_.at(r) = value;
}

void Model::write_reg(unsigned r, std::uint32_t v, RetireRecord& rec) {
  if (r == 0) return;
  regs_[r] = v;
  rec.rd = static_cast<std::uint8_t>(r);
  rec.rd_value = v;
}

// Sets the next pc to `target`; traps (and returns false) if it is not 4-byte aligned.
bool Model::jump_to(std::uint32_t target, RetireRecord& rec) {
  if ((target & 3u) != 0) {
    status_ = Status::Trap;
    return false;
  }
  rec.next_pc = target;
  return true;
}

void Model::do_load(const Decoded& d, RetireRecord& rec) {
  const std::uint32_t addr = regs_[d.rs1] + static_cast<std::uint32_t>(d.imm);
  const bool word = d.op == Op::LW;
  const bool half = d.op == Op::LH || d.op == Op::LHU;
  if ((word && (addr & 3u) != 0) || (half && (addr & 1u) != 0)) {
    status_ = Status::Trap;
    return;
  }
  const std::uint32_t base = addr & ~3u;
  if (!word_in_ram(base)) {
    status_ = Status::MemFault;
    return;
  }
  const std::uint32_t w = read_word(base);
  const unsigned shift = 8 * (addr & 3u);
  std::uint32_t v = 0;
  switch (d.op) {
    case Op::LW: v = w; break;
    case Op::LH: v = sext(w >> shift, 16); break;
    case Op::LHU: v = (w >> shift) & 0xffffu; break;
    case Op::LB: v = sext(w >> shift, 8); break;
    default: v = (w >> shift) & 0xffu; break;  // LBU
  }
  write_reg(d.rd, v, rec);
}

void Model::do_store(const Decoded& d) {
  const std::uint32_t addr = regs_[d.rs1] + static_cast<std::uint32_t>(d.imm);
  const std::uint32_t value = regs_[d.rs2];
  std::uint32_t data = 0;
  std::uint8_t strobe = 0;
  switch (d.op) {
    case Op::SW:
      if ((addr & 3u) != 0) { status_ = Status::Trap; return; }
      data = value;
      strobe = 0xf;
      break;
    case Op::SH:
      if ((addr & 1u) != 0) { status_ = Status::Trap; return; }
      data = (value & 0xffffu) * 0x00010001u;
      strobe = static_cast<std::uint8_t>(0x3u << (addr & 2u));
      break;
    default:  // SB
      data = (value & 0xffu) * 0x01010101u;
      strobe = static_cast<std::uint8_t>(1u << (addr & 3u));
      break;
  }
  const std::uint32_t base = addr & ~3u;
  if (base == kMagicAddr) {
    status_ = Status::Exited;
    exit_code_ = data;
    return;
  }
  if (!word_in_ram(base)) {
    status_ = Status::MemFault;
    return;
  }
  for (unsigned b = 0; b < 4; ++b)
    if ((strobe & (1u << b)) != 0) ram_[base + b] = static_cast<std::uint8_t>(data >> (8 * b));
  if (cfg_.record_trace) stores_.push_back({base, data, strobe});
}

Status Model::step() {
  if (status_ != Status::Running) return status_;
  if (instret_ >= cfg_.max_instructions) return status_ = Status::Timeout;

  // Fetch. Alignment is checked before range, like the core.
  if ((pc_ & 3u) != 0) return status_ = Status::Trap;
  if (!word_in_ram(pc_)) return status_ = Status::MemFault;
  const std::uint32_t insn = read_word(pc_);
  const Decoded d = decode(insn);

  RetireRecord rec;
  rec.pc = pc_;
  rec.insn = insn;
  rec.next_pc = pc_ + 4;

  const std::uint32_t a = regs_[d.rs1];
  const std::uint32_t b = regs_[d.rs2];
  const std::uint32_t imm = static_cast<std::uint32_t>(d.imm);
  bool taken = false;
  rec.rs1_value = reads_rs1(d.op) ? a : 0;
  rec.rs2_value = reads_rs2(d.op) ? b : 0;

  switch (d.op) {
    case Op::LUI: write_reg(d.rd, imm, rec); break;
    case Op::AUIPC: write_reg(d.rd, pc_ + imm, rec); break;
    case Op::JAL:
      if (jump_to(pc_ + imm, rec)) write_reg(d.rd, pc_ + 4, rec);
      break;
    case Op::JALR:
      // Target uses rs1 before rd is written, so rd == rs1 is safe. Bit 0 of the target is cleared.
      if (jump_to((a + imm) & ~1u, rec)) write_reg(d.rd, pc_ + 4, rec);
      break;
    case Op::BEQ: taken = a == b; break;
    case Op::BNE: taken = a != b; break;
    case Op::BLT: taken = as_signed(a) < as_signed(b); break;
    case Op::BGE: taken = as_signed(a) >= as_signed(b); break;
    case Op::BLTU: taken = a < b; break;
    case Op::BGEU: taken = a >= b; break;
    case Op::LB: case Op::LH: case Op::LW: case Op::LBU: case Op::LHU: do_load(d, rec); break;
    case Op::SB: case Op::SH: case Op::SW: do_store(d); break;
    case Op::ADDI: write_reg(d.rd, a + imm, rec); break;
    case Op::SLTI: write_reg(d.rd, as_signed(a) < as_signed(imm) ? 1 : 0, rec); break;
    case Op::SLTIU: write_reg(d.rd, a < imm ? 1 : 0, rec); break;
    case Op::XORI: write_reg(d.rd, a ^ imm, rec); break;
    case Op::ORI: write_reg(d.rd, a | imm, rec); break;
    case Op::ANDI: write_reg(d.rd, a & imm, rec); break;
    case Op::SLLI: write_reg(d.rd, a << (imm & 31u), rec); break;
    case Op::SRLI: write_reg(d.rd, a >> (imm & 31u), rec); break;
    case Op::SRAI: write_reg(d.rd, sra(a, imm & 31u), rec); break;
    case Op::ADD: write_reg(d.rd, a + b, rec); break;
    case Op::SUB: write_reg(d.rd, a - b, rec); break;
    case Op::SLL: write_reg(d.rd, a << (b & 31u), rec); break;
    case Op::SLT: write_reg(d.rd, as_signed(a) < as_signed(b) ? 1 : 0, rec); break;
    case Op::SLTU: write_reg(d.rd, a < b ? 1 : 0, rec); break;
    case Op::XOR: write_reg(d.rd, a ^ b, rec); break;
    case Op::SRL: write_reg(d.rd, a >> (b & 31u), rec); break;
    case Op::SRA: write_reg(d.rd, sra(a, b & 31u), rec); break;
    case Op::OR: write_reg(d.rd, a | b, rec); break;
    case Op::AND: write_reg(d.rd, a & b, rec); break;
    case Op::FENCE: break;
    case Op::ECALL: case Op::EBREAK: case Op::Illegal: status_ = Status::Trap; break;
  }

  if (d.op >= Op::BEQ && d.op <= Op::BGEU && taken) jump_to(pc_ + imm, rec);

  if (status_ == Status::Running || status_ == Status::Exited) {
    ++instret_;
    if (cfg_.record_trace) retired_.push_back(rec);
    pc_ = rec.next_pc;
  }
  return status_;
}

Status Model::run() {
  while (step() == Status::Running) {
  }
  return status_;
}

ModelResult Model::result() const {
  ModelResult r;
  r.status = status_;
  r.exit_code = exit_code_;
  r.instret = instret_;
  r.pc = pc_;
  r.regs = regs_;
  r.mem_hash = hash_ram(ram_);
  r.retired = retired_;
  r.stores = stores_;
  return r;
}

ModelResult run_model(std::span<const std::uint32_t> code, const ModelConfig& cfg) {
  Model m(cfg);
  m.load(code);
  m.run();
  return m.result();
}

}  // namespace cutoff
