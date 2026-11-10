`timescale 1ns/1ps
module late_effects_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] seed = 0, saved_seed;
  wire [3:0] input_r_address0, output_r_address0;
  wire [5:0] audit_address0;
  wire input_r_ce0, output_r_ce0, output_r_we0, audit_ce0, audit_we0;
  wire [31:0] output_r_d0, audit_d0;
  reg [31:0] input_r_q0 = 0;
  reg [63:0] audit_seen;
  reg [15:0] output_seen;
  reg [31:0] expected;
  integer trial, cycles, writes = 0, audit_writes = 0, completions = 0, tick = 0;
  late_effects dut (.*);
  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    tick = tick + 1;
    if (!ap_rst) begin
      if (input_r_ce0) input_r_q0 <= input_r_address0 * 997 + 13;
      if (output_r_ce0 && output_r_we0) begin
        expected = ((output_r_address0 * 997 + 13) + saved_seed) ^ 32'h125678ab;
        if (output_seen[output_r_address0] || output_r_d0 !== expected)
          $fatal(1, "Output mismatch or duplicate");
        output_seen[output_r_address0] = 1;
        writes = writes + 1;
      end
      if (audit_ce0 && audit_we0) begin
        expected = saved_seed ^ (audit_address0 * 17);
        if (audit_seen[audit_address0] || audit_d0 !== expected)
          $fatal(1, "Trailing memory effect mismatch or duplicate");
        audit_seen[audit_address0] = 1;
        audit_writes = audit_writes + 1;
      end
      if (ap_done) begin
        if (writes != 16 || audit_writes != 64) $fatal(1, "Early done before trailing writes");
        completions = completions + 1;
      end
    end
  end
  always @(negedge ap_clk) begin
`ifdef INJECT_STALLS
    if (!ap_rst && !dut.produce_tail_0_idle && tick % 11 < 4)
      force dut.produce_tail_0_clock_enable = 1'b0;
    else
      release dut.produce_tail_0_clock_enable;
`endif
  end
  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    for (trial = 0; trial < 64; trial = trial + 1) begin
      writes = 0; audit_writes = 0; completions = 0; audit_seen = 0; output_seen = 0;
      saved_seed = (trial * 971 + 17) ^ 32'haf359367;
      seed = saved_seed; ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0; seed = ~saved_seed;
      cycles = 0;
      while (completions < 1 && cycles < 1000) begin
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (completions != 1 || audit_seen !== {64{1'b1}} || output_seen !== {16{1'b1}})
        $fatal(1, "Incomplete transaction");
      repeat (3) @(negedge ap_clk);
      if (completions != 1 || writes != 16 || audit_writes != 64)
        $fatal(1, "Effects after completion");
    end
    $display("DATAFLOW COMPLETION TESTS PASSED: 64 calls, 5120 committed writes");
    $finish;
  end
endmodule
