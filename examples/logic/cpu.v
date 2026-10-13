// The processor the logic simulator runs: PicoRV32 as its Dhrystone
// testbench sets it up (a barrel shifter, a fast multiplier, a divider;
// it starts at 0x10000), with nothing but its memory bus brought out.
// The memory itself is no part of the circuit: whoever simulates it
// answers the bus (examples/logic/cpu.ent, and the testbenches made by
// check/cpu.py).
module cpu(
    input clk, input resetn, output trap,
    output mem_valid, output mem_instr, input mem_ready,
    output [31:0] mem_addr, output [31:0] mem_wdata, output [3:0] mem_wstrb,
    input [31:0] mem_rdata);
  picorv32 #(
    .BARREL_SHIFTER(1), .ENABLE_FAST_MUL(1), .ENABLE_DIV(1),
    .PROGADDR_RESET(32'h0001_0000), .STACKADDR(32'h0001_0000)
  ) core (
    .clk(clk), .resetn(resetn), .trap(trap),
    .mem_valid(mem_valid), .mem_instr(mem_instr), .mem_ready(mem_ready),
    .mem_addr(mem_addr), .mem_wdata(mem_wdata), .mem_wstrb(mem_wstrb),
    .mem_rdata(mem_rdata),
    .pcpi_wr(1'b0), .pcpi_rd(32'b0), .pcpi_wait(1'b0), .pcpi_ready(1'b0),
    .irq(32'b0));
endmodule
