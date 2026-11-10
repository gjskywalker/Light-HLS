`timescale 1ns/1ps
module scalar_returns_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] a, b, mode;
  wire [31:0] ap_return;
  reg [31:0] expected, prior, accepted_a, accepted_b, accepted_mode, held;
  integer trial, i, trips, cycles, total_cycles = 0;
  scalar_returns dut (.*);
  always #5 ap_clk = ~ap_clk;
  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    for (trial = 0; trial < 64; trial = trial + 1) begin
      accepted_a = 32'hfedc0123 ^ (trial * 32'h13572189);
      accepted_b = 32'h87ac5234 ^ (trial * 32'h4e171851);
      accepted_mode = trial;
      a = accepted_a;
      b = accepted_b;
      mode = accepted_mode;
      expected = (mode & 1) ? a : b;
      prior = expected;
      trips = `CASE_KIND == 1 ? mode & 15 : 4;
      for (i = 0; i < trips; i = i + 1) begin
        prior = expected;
        expected = expected * 3 + (a ^ i);
      end
      if (`CASE_KIND == 2) expected = prior ^ (expected + b);
      else expected = (mode & 2) ? expected - b : expected + b;
      ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0;
      a = ~accepted_a;
      b = ~accepted_b;
      mode = ~accepted_mode;
      cycles = 0;
      while (!ap_done && cycles < 1000) begin
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (!ap_done || !ap_ready || !ap_idle || ap_return !== expected)
        $fatal(1, "Return mismatch trial=%0d expected=%h actual=%h cycles=%0d", trial, expected, ap_return, cycles);
      total_cycles = total_cycles + cycles;
      held = ap_return;
      if (trial & 1) begin
        repeat (2) begin
          @(negedge ap_clk);
          if (ap_done || ap_return !== held) $fatal(1, "Completed result must remain held");
        end
      end
    end
    $display("SCALAR RETURN TESTS PASSED: 64 calls, total_cycles=%0d", total_cycles);
    $finish;
  end
endmodule
