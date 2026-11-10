`timescale 1ns/1ps

module memory_request_order_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  reg [31:0] seed;
  wire ap_done, ap_idle, ap_ready;
  wire [31:0] ap_return;
  wire [2:0] memory_address0, memory_address1;
  wire memory_ce0, memory_ce1, memory_we0, memory_we1;
  wire [31:0] memory_d0, memory_d1;
  reg [31:0] memory_q0 = 0, memory_q1 = 0;
  reg [31:0] storage [0:7];
  reg [31:0] expected;
  integer trial, index, cycles, writes;

  memory_request_order dut (.*);
  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    if (memory_ce0) begin
      if (memory_we0) begin
        storage[memory_address0] <= memory_d0;
        writes = writes + 1;
      end else memory_q0 <= storage[memory_address0];
    end
    if (memory_ce1) begin
      if (memory_we1) begin
        storage[memory_address1] <= memory_d1;
        writes = writes + 1;
      end else memory_q1 <= storage[memory_address1];
    end
  end
  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    // Exercise every possible alias, including the overwritten address, and
    // restart without reset so stale values cannot mask an ordering failure.
    for (trial = 0; trial < 64; trial = trial + 1) begin
      for (index = 0; index < 8; index = index + 1)
        storage[index] = (trial + 1) * 37 + index * 11;
      storage[0] = trial & 7;
      expected = storage[trial & 7];
      seed = 32'hcafe0000 + trial;
      writes = 0;
      ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0;
      cycles = 0;
      while (!ap_done && cycles < 100) begin
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (!ap_done || ap_return !== expected || storage[7] !== seed || writes != 1)
        $fatal(1, "Memory request order: trial=%0d expected=%0d actual=%0d writes=%0d",
               trial, expected, ap_return, writes);
      @(negedge ap_clk);
    end
    $display("ALL MEMORY REQUEST ORDER TESTS PASSED (64 transactions)");
    $finish;
  end
endmodule
