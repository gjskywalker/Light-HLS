`timescale 1ns/1ps
// Score transactions at the same edge that captures scalar arguments.
module held_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] seed = 0, count = 0;
  wire [3:0] output_r_address0;
  wire output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  integer accepted = 0, completed = 0, writes = 0, tick = 0;
  integer mode = 0, trips, i;
  reg checking = 0;
  reg [31:0] expected_seed, x, expected;
  transaction_edges dut (.*);
  always #5 ap_clk = ~ap_clk;

  function [31:0] argument_for(input integer transaction);
    argument_for = 32'h8123abcd ^ (transaction * 32'h4f013217);
  endfunction

  always @(posedge ap_clk) begin
    tick = tick + 1;
    if (ap_rst && (ap_ready || output_r_ce0 || output_r_we0))
      $fatal(1, "Reset exposed a request or a memory side effect");
    if (checking && !ap_rst) begin
      expected_seed = argument_for(completed);
      trips = `CASE_KIND == 2 ? completed % 16 : 16;
      if (`CASE_KIND == 3 && (expected_seed & 1)) trips = 0;
      if (output_r_ce0 && output_r_we0) begin
        if (completed >= accepted || writes >= trips || output_r_address0 !== writes[3:0])
          $fatal(1, "Unexpected, duplicated or reordered output");
        x = `CASE_KIND == 1 ? expected_seed : 7;
        for (i = 0; i <= writes; i = i + 1) begin
          if (`CASE_KIND == 4)
            x = $unsigned($signed(x) >>> 3) ^ (x << 5) ^ 32'h9e3779b9;
          else if (`CASE_KIND == 5)
            x = (x << (expected_seed & 7)) ^ (x >> ((expected_seed >> 4) & 7)) ^ 32'h9e3779b9;
          else x = (x << 1) ^ (x >> 1) ^ 32'h9e3779b9;
        end
        expected = x ^ expected_seed;
        if (output_r_d0 !== expected)
          $fatal(1, "Scalar capture mismatch transaction=%0d write=%0d", completed, writes);
        writes = writes + 1;
      end
      if (ap_done) begin
        if (completed >= accepted || writes != trips)
          $fatal(1, "Lost, duplicated or premature completion");
        completed = completed + 1;
        writes = 0;
      end
      if (ap_start && ap_ready) begin
        if (accepted >= 64) $fatal(1, "Unexpected acceptance");
        accepted = accepted + 1;
      end
    end
  end

  task drive_stream;
    begin
      while (completed < 64 && tick < 50000) begin
        @(negedge ap_clk);
        seed = argument_for(accepted);
        count = accepted;
        ap_start = accepted < 64 && (mode == 0 || tick % 7 < 4);
      end
      ap_start = 0;
      if (completed != 64 || accepted != 64) $fatal(1, "Stream timed out");
      repeat (8) @(negedge ap_clk);
      if (!ap_idle || !ap_ready || ap_done || writes || output_r_ce0)
        $fatal(1, "Effects after stream drain");
    end
  endtask

  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 0; seed = 32'habcdef02; count = 15; ap_start = 1;
    @(negedge ap_clk); ap_start = 0;
    // Interrupt active work; aborted transactions must not complete later.
    repeat (5) @(negedge ap_clk);
    ap_rst = 1; ap_start = 1;
    repeat (3) @(negedge ap_clk);
    ap_start = 0; ap_rst = 0; checking = 1;
    drive_stream;
    mode = 1; accepted = 0; completed = 0;
    drive_stream;
    $display("HELD TRANSACTION TESTS PASSED: 128 calls and reset recovery");
    $finish;
  end
endmodule
