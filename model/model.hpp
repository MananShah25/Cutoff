#pragma once
// RV32I reference model. It mirrors the testbench of the core under test: same memory map, same
// reset state, same termination rule, so differences between model and core are real findings.
//
// Semantics fixed here (each is tested):
//  - x0 reads as zero and ignores writes; all registers and RAM start at zero.
//  - Control-flow targets must be 4-byte aligned (no compressed instructions): a taken branch,
//    JAL or JALR to any other target traps. Not-taken branches never trap.
//  - Misaligned LH/LHU/SH and LW/SW trap; byte accesses never do. Alignment is checked before range.
//  - A store whose word address is kMagicAddr ends the run; exit_code is the bus data word
//    (SB replicates the byte into all four lanes, SH replicates the halfword into both halves).
//  - Any other access outside RAM, including an instruction fetch, is a MemFault.
//  - FENCE is a no-op. ECALL, EBREAK, CSR instructions and anything else unsupported trap.
//  - On Trap or MemFault the register file is unspecified and must not be compared.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "memory_map.hpp"
#include "rv32i.hpp"

namespace cutoff {

struct ModelConfig {
  std::uint64_t max_instructions = 100'000;
  bool record_trace = false;
};

// One retired instruction. rd is 0 when nothing was written (including writes to x0).
struct RetireRecord {
  std::uint32_t pc = 0, insn = 0, next_pc = 0;
  std::uint8_t rd = 0;
  std::uint32_t rd_value = 0;
  std::uint32_t rs1_value = 0;  // value read from rs1 before the instruction ran; 0 if the kind does not read rs1
  std::uint32_t rs2_value = 0;  // likewise for rs2
};

struct ModelResult {
  Status status = Status::Running;
  std::uint32_t exit_code = 0;
  std::uint64_t instret = 0;  // instructions retired, including the terminating store
  std::uint32_t pc = 0;
  std::array<std::uint32_t, 32> regs{};
  std::uint64_t mem_hash = 0;
  std::vector<RetireRecord> retired;
  std::vector<StoreRecord> stores;
};

class Model {
 public:
  explicit Model(ModelConfig cfg = {});

  void load(std::span<const std::uint32_t> code);  // at address 0
  void write_word(std::uint32_t addr, std::uint32_t value);
  std::uint32_t read_word(std::uint32_t addr) const;
  void set_reg(unsigned r, std::uint32_t value);
  void set_pc(std::uint32_t pc) { pc_ = pc; }

  Status step();  // executes one instruction; returns the status afterwards
  Status run();   // steps until the status is not Running
  ModelResult result() const;

  std::uint32_t reg(unsigned r) const { return regs_[r]; }
  std::uint32_t pc() const { return pc_; }
  Status status() const { return status_; }

 private:
  bool aligned_fetch_ok();
  void write_reg(unsigned r, std::uint32_t v, RetireRecord& rec);
  bool jump_to(std::uint32_t target, RetireRecord& rec);
  void do_load(const Decoded& d, RetireRecord& rec);
  void do_store(const Decoded& d);

  ModelConfig cfg_;
  std::array<std::uint32_t, 32> regs_{};
  std::uint32_t pc_ = 0;
  std::vector<std::uint8_t> ram_;
  Status status_ = Status::Running;
  std::uint32_t exit_code_ = 0;
  std::uint64_t instret_ = 0;
  std::vector<RetireRecord> retired_;
  std::vector<StoreRecord> stores_;
};

ModelResult run_model(std::span<const std::uint32_t> code, const ModelConfig& cfg = {});

}  // namespace cutoff
