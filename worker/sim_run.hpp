#pragma once
// The cycle loop and the memory slave shared by the original core model and the instrumented (mutant) core model.
// `Top` is a Verilated model class; `Traits` supplies the two things that differ between them:
//   static void select(Top&, std::uint32_t mutant);          // drive mut_sel (a no-op for the original core)
//   static std::uint32_t reg(Top&, std::size_t index);       // read architectural register x<index>
// The including .cpp must have included the Verilator headers of `Top` first.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "memory_map.hpp"
#include "sim_core.hpp"

namespace cutoff::detail {

constexpr int kResetCycles = 8;
constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

inline void mix(std::uint64_t& h, std::uint64_t v) {
  for (unsigned i = 0; i < 8; ++i) {
    h ^= (v >> (8 * i)) & 0xffu;
    h *= kFnvPrime;
  }
}



template <class Top, class Traits>
RunResult run_program_t(std::span<const std::uint32_t> code, const RunConfig& cfg, std::uint32_t mutant) {
  RunResult res;
  res.trace_hash = kFnvOffset;

  std::vector<std::uint8_t> ram(kRamBytes, 0);
  for (std::size_t i = 0; i < code.size() && 4 * i + 3 < ram.size(); ++i)
    for (unsigned b = 0; b < 4; ++b) ram[4 * i + b] = static_cast<std::uint8_t>(code[i] >> (8 * b));

  auto top = std::make_unique<Top>();
  Traits::select(*top, mutant);
  top->clk = 0;
  top->resetn = 0;
  top->mem_ready = 0;
  top->mem_rdata = 0;

  bool done = false;
  bool pending = false;  // a bus event ended the run; waiting out the trap window
  Status pending_status = Status::Timeout;
  unsigned settle_left = 0;
  for (std::uint64_t cycle = 0; !done; ++cycle) {
    if (cycle >= cfg.max_cycles) {
      res.status = Status::Timeout;
      break;
    }
    if (cycle >= kResetCycles) top->resetn = 1;

    // Memory responds like a registered slave: it looks at the outputs the core
    // showed before this clock edge and drives ready/rdata for the next one.
    bool next_ready = false;
    std::uint32_t next_rdata = top->mem_rdata;
    if (!pending && top->resetn && top->mem_valid && !top->mem_ready) {
      const std::uint32_t addr = top->mem_addr & ~3u;
      const std::uint8_t wstrb = top->mem_wstrb;
      const bool is_write = wstrb != 0;
      if (is_write && addr == kMagicAddr) {
        pending = true;
        pending_status = Status::Exited;
        res.exit_code = top->mem_wdata;
      } else if (addr + 3 >= kRamBytes) {
        pending = true;
        pending_status = Status::MemFault;
      } else {
        next_rdata = 0;
        for (unsigned b = 0; b < 4; ++b) next_rdata |= static_cast<std::uint32_t>(ram[addr + b]) << (8 * b);
        if (is_write)
          for (unsigned b = 0; b < 4; ++b)
            if ((wstrb & (1u << b)) != 0) ram[addr + b] = static_cast<std::uint8_t>(top->mem_wdata >> (8 * b));
        next_ready = true;
      }
      if (pending) settle_left = cfg.trap_settle_cycles;
      mix(res.trace_hash, cycle);
      mix(res.trace_hash, (static_cast<std::uint64_t>(top->mem_instr) << 4U) | wstrb);
      mix(res.trace_hash, top->mem_addr);
      mix(res.trace_hash, top->mem_wdata);
      mix(res.trace_hash, next_rdata);
    }

    top->clk = 0;
    top->eval();
    top->clk = 1;
    top->eval();
    top->mem_ready = next_ready;
    top->mem_rdata = next_rdata;
    res.cycles = cycle + 1;

    if (top->resetn && top->trap) {  // a trap preempts a pending bus event
      res.status = Status::Trap;
      done = true;
    } else if (pending) {
      if (settle_left == 0) {
        res.status = pending_status;
        done = true;
      } else {
        --settle_left;
      }
    }
  }

  res.mem_hash = hash_ram(ram);
  for (std::size_t i = 1; i < 32; ++i) res.regs[i] = Traits::reg(*top, i);
  top->final();
  return res;
}

}  // namespace cutoff::detail
