// Cutoff wrapper around the pinned PicoRV32. Fixes the RV32I configuration
// so picorv32.v itself stays unmodified (see docs/decisions.md D-004).
`timescale 1 ns / 1 ps

module cutoff_core (
	input         clk,
	input         resetn,
	output        trap,
	output        mem_valid,
	output        mem_instr,
	input         mem_ready,
	output [31:0] mem_addr,
	output [31:0] mem_wdata,
	output [ 3:0] mem_wstrb,
	input  [31:0] mem_rdata
`ifdef RISCV_FORMAL
	,output rvfi_valid,
	output [63:0] rvfi_order,
	output [31:0] rvfi_insn,
	output [31:0] rvfi_pc_rdata,
	output [4:0] rvfi_rd_addr,
	output [31:0] rvfi_rd_wdata,
	output [31:0] rvfi_mem_addr,
	output [3:0] rvfi_mem_rmask,
	output [3:0] rvfi_mem_wmask,
	output [31:0] rvfi_mem_rdata,
	output [31:0] rvfi_mem_wdata
`endif
);
	/* verilator lint_off UNUSEDSIGNAL */
	wire        la_read, la_write, pcpi_valid;
	wire [31:0] la_addr, la_wdata, pcpi_insn, pcpi_rs1, pcpi_rs2, eoi;
	wire [35:0] trace_data;
	wire [ 3:0] la_wstrb;
	wire        trace_valid;
	/* verilator lint_on UNUSEDSIGNAL */

	picorv32 #(
		.ENABLE_COUNTERS     (0),
		.ENABLE_COUNTERS64   (0),
		.ENABLE_REGS_16_31   (1),
		.ENABLE_REGS_DUALPORT(1),
		.LATCHED_MEM_RDATA   (0),
		.TWO_STAGE_SHIFT     (1),
		.BARREL_SHIFTER      (0),
		.TWO_CYCLE_COMPARE   (0),
		.TWO_CYCLE_ALU       (0),
		.COMPRESSED_ISA      (0),
		.CATCH_MISALIGN      (1),
		.CATCH_ILLINSN       (1),
		.ENABLE_PCPI         (0),
		.ENABLE_MUL          (0),
		.ENABLE_FAST_MUL     (0),
		.ENABLE_DIV          (0),
		.ENABLE_IRQ          (0),
		.ENABLE_TRACE        (0),
		.REGS_INIT_ZERO      (1),
		.PROGADDR_RESET      (32'h0000_0000),
		.STACKADDR           (32'hffff_ffff)
	) cpu (
		.clk      (clk),
		.resetn   (resetn),
		.trap     (trap),
		.mem_valid(mem_valid),
		.mem_instr(mem_instr),
		.mem_ready(mem_ready),
		.mem_addr (mem_addr),
		.mem_wdata(mem_wdata),
		.mem_wstrb(mem_wstrb),
		.mem_rdata(mem_rdata),
		.mem_la_read (la_read),
		.mem_la_write(la_write),
		.mem_la_addr (la_addr),
		.mem_la_wdata(la_wdata),
		.mem_la_wstrb(la_wstrb),
		.pcpi_valid  (pcpi_valid),
		.pcpi_insn   (pcpi_insn),
		.pcpi_rs1    (pcpi_rs1),
		.pcpi_rs2    (pcpi_rs2),
		.pcpi_wr     (1'b0),
		.pcpi_rd     (32'b0),
		.pcpi_wait   (1'b0),
		.pcpi_ready  (1'b0),
		.irq         (32'b0),
		.eoi         (eoi),
		.trace_valid (trace_valid),
		.trace_data  (trace_data)
	`ifdef RISCV_FORMAL
		,.rvfi_valid(rvfi_valid), .rvfi_order(rvfi_order), .rvfi_insn(rvfi_insn), .rvfi_pc_rdata(rvfi_pc_rdata),
		.rvfi_rd_addr(rvfi_rd_addr), .rvfi_rd_wdata(rvfi_rd_wdata), .rvfi_mem_addr(rvfi_mem_addr),
		.rvfi_mem_rmask(rvfi_mem_rmask), .rvfi_mem_wmask(rvfi_mem_wmask), .rvfi_mem_rdata(rvfi_mem_rdata), .rvfi_mem_wdata(rvfi_mem_wdata)
	`endif
	);
endmodule
