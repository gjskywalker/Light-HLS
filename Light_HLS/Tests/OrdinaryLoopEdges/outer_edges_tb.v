`timescale 1ns/1ps

module outer_edges_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] seed, count;
  wire [3:0] input_r_address0, output_r_address0;
  wire input_r_ce0, output_r_ce0, output_r_we0;
  reg [31:0] input_r_q0 = 0, delayed_read = 0;
  wire [31:0] output_r_d0;
  reg [31:0] source [0:15], result [0:15], expected [0:15];
  reg [31:0] random_state = 32'h68ac037d, sampled_seed, x, y, old_x;
  reg [15:0] seen, expected_seen;
  integer trial, index, j, limit, step, reads, writes, expected_events;
  integer cycles, total_cycles = 0, total_outputs = 0;
  outer_edges dut (.*);
  always #5 ap_clk = ~ap_clk;

  always @(posedge ap_clk) begin
    if (!ap_rst && input_r_ce0) begin
      if ((^input_r_address0) === 1'bx) $fatal(1, "Unknown read address");
      if (`READ_LATENCY == 2) delayed_read <= source[input_r_address0];
      else input_r_q0 <= source[input_r_address0];
      reads = reads + 1;
    end
    if (`READ_LATENCY == 2) input_r_q0 <= delayed_read;
    if (!ap_rst && output_r_ce0 && output_r_we0) begin
      if ((^output_r_address0) === 1'bx || seen[output_r_address0])
        $fatal(1, "Unknown or duplicate write address");
      seen[output_r_address0] = 1;
      result[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end
  end

  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    for (trial = 0; trial < 64; trial = trial + 1) begin
      for (index = 0; index < 16; index = index + 1) begin
        random_state = random_state ^ (random_state << 13);
        random_state = random_state ^ (random_state >> 17);
        random_state = random_state ^ (random_state << 5);
        source[index] = random_state;
        expected[index] = 32'hdeadbeef;
        result[index] = 32'hdeadbeef;
      end
      sampled_seed = random_state;
      limit = `CASE_KIND == 1 ? trial % 16 : (`CASE_KIND == 8 ? 1 : 16);
      step = `CASE_KIND == 3 ? (sampled_seed & 3) + 1 : 1;
      expected_seen = 0;
      expected_events = 0;
      for (index = 0; index < limit; index = index + step) begin
        x = source[index];
        if (`CASE_KIND == 8) y = source[1];
        if ((`CASE_KIND == 5 && (trial & 1)) || (`CASE_KIND == 9 && (x & 1))) begin
          for (j = 0; j < 4; j = j + 1) x = x ^ (x << 1);
        end else begin
          for (j = 0; j < 8; j = j + 1) begin
            if (`CASE_KIND == 8) begin
              old_x = x;
              x = (x << 1) ^ y;
              y = (y >> 1) ^ old_x;
            end else x = (x << 1) ^ (x >> 1) ^ 32'h9e3779b9;
          end
        end
        if (`CASE_KIND == 4) expected[index] = x ^ source[index];
        else if (`CASE_KIND == 6) expected[index] = x ^ (source[index] / (sampled_seed | 1));
        else if (`CASE_KIND == 8) expected[index] = x ^ y;
        else expected[index] = x ^ sampled_seed;
        expected_seen[index] = 1;
        expected_events = expected_events + 1;
      end
      seed = sampled_seed;
      count = trial;
      reads = 0;
      writes = 0;
      seen = 0;
      ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0;
      // Scalar inputs may change after the transaction has been accepted.
      seed = ~sampled_seed;
      count = 0;
      cycles = 0;
      while (!ap_done && cycles < 10000) begin
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (!ap_done || writes != expected_events ||
          reads != expected_events * (`CASE_KIND == 8 ? 2 : 1) || seen !== expected_seen)
        $fatal(1, "Control mismatch trial=%0d reads=%0d writes=%0d cycles=%0d", trial, reads, writes, cycles);
      for (index = 0; index < 16; index = index + 1)
        if (result[index] !== expected[index])
          $fatal(1, "Output mismatch trial=%0d index=%0d expected=%h actual=%h", trial, index, expected[index], result[index]);
      total_cycles = total_cycles + cycles;
      total_outputs = total_outputs + writes;
      repeat (3) @(negedge ap_clk);
    end
    $display("OUTER EDGE TESTS PASSED: 64 calls, outputs=%0d, total_cycles=%0d", total_outputs, total_cycles);
    $finish;
  end
endmodule
