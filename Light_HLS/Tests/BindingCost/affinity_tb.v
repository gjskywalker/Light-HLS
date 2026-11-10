`timescale 1ns/1ps
module affinity_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] a = 0, b = 0, x = 0, y = 0;
  wire [31:0] ap_return;
  reg [31:0] p, q, r, s, expected, previous = 0, random_state = 32'h21431578;
  integer trial, cycle;
  binding_affinity dut (.*);
  always #5 ap_clk = ~ap_clk;
  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    for (trial = 0; trial < 256; trial = trial + 1) begin
      random_state = random_state ^ (random_state << 13);
      random_state = random_state ^ (random_state >> 17);
      random_state = random_state ^ (random_state << 5);
      a = random_state; b = ~random_state; x = trial * 971 + 17; y = trial * 167 + 31;
      p = a * x; q = b * y; r = b * (p ^ 7); s = a * (q ^ 9); expected = r ^ s;
      ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0;
      a = ~a; b = ~b; x = ~x; y = ~y;
      cycle = 0;
      while (!ap_done && cycle < 100) begin
        if (ap_return !== previous) $fatal(1, "Return-hold violation");
        @(negedge ap_clk);
        cycle = cycle + 1;
      end
      if (!ap_done || ap_return !== expected)
        $fatal(1, "Binding mismatch trial=%0d expected=%h actual=%h", trial, expected, ap_return);
      previous = ap_return;
      @(negedge ap_clk);
    end
    $display("BINDING COST TESTS PASSED: 256 calls");
    $finish;
  end
endmodule
