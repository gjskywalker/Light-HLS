`timescale 1ns/1ps
module testbench;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  reg [31:0] a = 0, b = 0, c = 0, mode = 0;
  wire ap_done, ap_idle, ap_ready;
  wire [31:0] ap_return;
  reg [31:0] expected, seed = 32'hb7c91e25;
  integer trial, watchdog;
  arithmetic_mapping dut (.*);
  always #5 ap_clk = ~ap_clk;
  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    for (trial = 0; trial < 1024; trial = trial + 1) begin
      seed = seed * 1664525 + 1013904223;
      a = trial < 4 ? 32'hffffffff : seed;
      seed = seed * 1664525 + 1013904223;
      b = trial < 4 ? 32'hffffffff : seed;
      c = seed ^ 32'haaaaaaaa;
      mode = trial & 1;
      expected = mode ? (a & 32'h1ffff) * (b & 32'h1ffff) + a : c * b + a;
      ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0;
      watchdog = 0;
      while (!ap_done && watchdog < 100) begin
        @(negedge ap_clk);
        watchdog = watchdog + 1;
      end
      if (!ap_done || ap_return !== expected)
        $fatal(1, "Arithmetic mismatch trial=%0d actual=%h expected=%h", trial, ap_return, expected);
      @(negedge ap_clk);
    end
    $display("ARITHMETIC TESTS PASSED: 1024 transactions");
    $finish;
  end
endmodule
