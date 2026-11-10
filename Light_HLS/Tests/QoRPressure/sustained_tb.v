`timescale 1ns/1ps
// Identical external traffic for both generators. Ready acknowledges a held
// offer; final memory commits define throughput, independently of early done.
module sustained_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] coefficient = 7;
  wire [7:0] input_r_address0, output_r_address0;
  wire input_r_ce0, output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] input_r_q0 = 0;
`ifdef PRESSURE_MEMORY_DIRECTIONS
  wire input_r_we0;
  wire [31:0] input_r_d0;
  reg [31:0] output_r_q0 = 0;
`endif
  integer tick = 0, acknowledged = 0, completed = 0, writes = 0, reads = 0;
  integer acknowledgements [0:31], finishes [0:31], commits [0:31];
  integer index;
  reg [31:0] expected, word;
  reg recording = 1;
  qor_pressure dut (.*);
  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    tick = tick + 1;
    if (ap_rst && (input_r_ce0 || output_r_ce0 || output_r_we0))
      $fatal(1, "Memory effect during reset");
    if (!ap_rst) begin
      if (ap_start && ap_ready) begin
        if (acknowledged >= 32) $fatal(1, "Unexpected acknowledgement");
        acknowledgements[acknowledged] = tick;
        acknowledged = acknowledged + 1;
      end
      if (input_r_ce0) begin
        if ((^input_r_address0) === 1'bx) $fatal(1, "Unknown read address");
        input_r_q0 <= (input_r_address0 * 131) ^ 32'ha531e297;
        reads = reads + 1;
      end
`ifdef PRESSURE_MEMORY_DIRECTIONS
      if (input_r_ce0 && input_r_we0) $fatal(1, "Unexpected input write");
`endif
      if (output_r_ce0 && output_r_we0) begin
        if (writes >= 8192 || output_r_address0 !== (writes % 256))
          $fatal(1, "Unexpected output address or duplicate write: %0d", writes);
        word = (output_r_address0 * 131) ^ 32'ha531e297;
        expected = (word & 32'h1ffff) * ((writes / 256) * 29 + 7) + output_r_address0;
        if (output_r_d0 !== expected) $fatal(1, "Sustained output mismatch: %0d", writes);
        commits[writes / 256] = tick;
        if (recording) $display("RESULT %0d %0d %0d", writes / 256, writes % 256, output_r_d0);
        writes = writes + 1;
      end
      if (ap_done) begin
        if (completed >= 32) $fatal(1, "Unexpected completion");
        finishes[completed] = tick;
        completed = completed + 1;
      end
    end
  end
  always @(negedge ap_clk) begin
    if (!ap_rst) begin
      ap_start = acknowledged < 32;
      coefficient = acknowledged * 29 + 7;
    end
  end
  initial begin
`ifdef RESET_ABORT
    recording = 0;
`endif
    repeat (3) @(negedge ap_clk);
    #1 ap_rst = 0;
`ifdef RESET_ABORT
    while (reads < 16 && tick < 1000) @(negedge ap_clk);
    if (reads < 16) $fatal(1, "Reset probe failed to enter active work");
    // Abort a partially executed pipeline, then restart with a clean oracle.
    ap_rst = 1;
    repeat (3) @(negedge ap_clk);
    acknowledged = 0; completed = 0; writes = 0; reads = 0;
    coefficient = 7; ap_start = 0; input_r_q0 = 0; recording = 1;
    #1 ap_rst = 0;
`endif
    while ((completed < 32 || writes < 8192) && tick < 30000) @(negedge ap_clk);
    if (completed != 32 || acknowledged != 32 || writes != 8192)
      $fatal(1, "Incomplete sustained sequence");
    repeat (8) @(negedge ap_clk);
    if (!ap_idle || ap_done || writes != 8192 || reads != 8192)
      $fatal(1, "Effects after drain or invalid read count: %0d", reads);
    for (index = 0; index < 32; index = index + 1)
      $display("SUSTAINED trial=%0d acknowledged=%0d last_commit=%0d done=%0d",
               index, acknowledgements[index], commits[index], finishes[index]);
    $display("SUSTAINED TESTS PASSED");
    $finish;
  end
endmodule
