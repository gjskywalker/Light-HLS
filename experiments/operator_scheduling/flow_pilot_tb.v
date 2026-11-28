`timescale 1ns/1ps

module pilot_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  wire [31:0] ap_return;
  wire [2:0] memory_address0;
  wire memory_ce0;
  reg [31:0] memory_q0 = 0, key = 0;
  reg [31:0] memory [0:7];
  reg [319:0] vectors [0:31];
  string vectors_path;
  integer row, address, cycles, first_cycles;

  `CASE_TOP dut (.*);
  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk)
    if (memory_ce0) memory_q0 <= memory[memory_address0];

  initial begin
    if (!$value$plusargs("vectors=%s", vectors_path)) $fatal(1, "missing vectors file");
    $readmemh(vectors_path, vectors);
    first_cycles = -1;
    for (row = 0; row < 32; row = row + 1) begin
      @(negedge ap_clk);
      ap_rst = 1;
      for (address = 0; address < 8; address = address + 1)
        memory[address] = vectors[row][32 * address +: 32];
      key = vectors[row][287:256];
      @(negedge ap_clk);
      ap_rst = 0;
      ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0;
      cycles = 0;
      while (ap_done !== 1'b1 && cycles < 256) begin
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (ap_done !== 1'b1 || ap_return !== vectors[row][319:288])
        $fatal(1, "case %0d cycles %0d expected %h got %h", row, cycles, vectors[row][319:288], ap_return);
      if (first_cycles < 0) first_cycles = cycles;
      if (first_cycles != cycles) $fatal(1, "unexpected data-dependent latency");
    end
    $display("RTL_CYCLES=%0d VECTORS=32", first_cycles);
    $finish;
  end
endmodule
