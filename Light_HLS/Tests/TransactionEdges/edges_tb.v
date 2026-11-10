`timescale 1ns/1ps
module transaction_edges_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] seed, count;
  wire [3:0] output_r_address0;
  wire output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] result [0:15], expected [0:15];
  reg [31:0] sampled_seed, x;
  reg [15:0] seen;
  integer trial, i, trips, writes, cycles, total_cycles = 0;
  transaction_edges dut (.*);
  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    if (!ap_rst && output_r_ce0 && output_r_we0) begin
      if ((^output_r_address0) === 1'bx || seen[output_r_address0])
        $fatal(1, "Invalid or duplicate output address");
      seen[output_r_address0] = 1;
      result[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end
  end
  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    for (trial = 0; trial < 64; trial = trial + 1) begin
      sampled_seed = 32'h8123abcd ^ (trial * 32'h4f013217);
      seed = sampled_seed;
      count = trial;
      trips = `CASE_KIND == 2 ? trial % 16 : 16;
      if (`CASE_KIND == 3 && (seed & 1)) trips = 0;
      x = `CASE_KIND == 1 ? sampled_seed : 7;
      for (i = 0; i < 16; i = i + 1) begin
        result[i] = 32'hdeadbeef;
        expected[i] = 32'hdeadbeef;
        if (i < trips) begin
          if (`CASE_KIND == 4)
            x = $unsigned($signed(x) >>> 3) ^ (x << 5) ^ 32'h9e3779b9;
          else if (`CASE_KIND == 5)
            x = (x << (sampled_seed & 7)) ^ (x >> ((sampled_seed >> 4) & 7)) ^ 32'h9e3779b9;
          else x = (x << 1) ^ (x >> 1) ^ 32'h9e3779b9;
          expected[i] = x ^ seed;
        end
      end
      writes = 0;
      seen = 0;
      ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0;
      seed = ~sampled_seed;
      count = 0;
      cycles = 0;
      while (!ap_done && cycles < 1000) begin
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (!ap_done || !ap_ready || !ap_idle || writes != trips)
        $fatal(1, "Transaction boundary mismatch trial=%0d writes=%0d expected=%0d", trial, writes, trips);
      for (i = 0; i < 16; i = i + 1)
        if (result[i] !== expected[i]) $fatal(1, "Incorrect output trial=%0d index=%0d", trial, i);
      total_cycles = total_cycles + cycles;
      // Accept the next transaction directly from the completed idle state.
    end
    $display("TRANSACTION EDGE TESTS PASSED: 64 calls, total_cycles=%0d", total_cycles);
    $finish;
  end
endmodule
