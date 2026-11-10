`timescale 1ns/1ps

// Light-HLS protocol stress: queued scalar arguments, retirement/accept overlap,
// and child backpressure. This is not a Vitis throughput comparison.
module queued_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] coefficient = 0;
  wire [7:0] input_r_address0, output_r_address0;
  wire input_r_ce0, output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] input_r_q0 = 0;
  reg [31:0] expected, word;
  integer tick = 0, accepted = 0, completed = 0, writes = 0, reads = 0;
  integer accepts [0:31], finishes [0:31], commits [0:31];
  integer overlaps = 0, index;
  qor_pressure dut (.*);
  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    tick = tick + 1;
    if (!ap_rst) begin
      if (ap_start && ap_ready) begin
        if (accepted >= 32) $fatal(1, "Unexpected acceptance");
        accepts[accepted] = tick;
        accepted = accepted + 1;
        if (ap_done) overlaps = overlaps + 1;
      end
      if (input_r_ce0) begin
        input_r_q0 <= (input_r_address0 * 131) ^ 32'ha531e297;
        reads = reads + 1;
      end
      if (output_r_ce0 && output_r_we0) begin
        if (writes >= 8192 || output_r_address0 != writes % 256)
          $fatal(1, "Unexpected output address or duplicate write: %0d", writes);
        word = (output_r_address0 * 131) ^ 32'ha531e297;
        expected = (word & 32'h1ffff) * ((writes / 256) * 29 + 7) + output_r_address0;
        if (output_r_d0 !== expected) $fatal(1, "Queued scalar argument mismatch: %0d", writes);
        commits[writes / 256] = tick;
        writes = writes + 1;
      end
      if (ap_done) begin
        if (completed >= accepted || writes < (completed + 1) * 256)
          $fatal(1, "Completion before committed effects");
        finishes[completed] = tick;
        completed = completed + 1;
      end
      if (accepted - completed > 2) $fatal(1, "Queue capacity exceeded");
    end
  end
  always @(negedge ap_clk) begin
    if (!ap_rst) begin
      ap_start = accepted < 32;
      coefficient = accepted * 29 + 7;
`ifdef INJECT_STALLS
      if (!dut.store_pressure_2_idle && tick % 17 < 5)
        force dut.store_pressure_2_clock_enable = 1'b0;
      else
        release dut.store_pressure_2_clock_enable;
`endif
    end
  end
  initial begin
    repeat (3) @(negedge ap_clk);
    #1 ap_rst = 0;
    while (completed < 32 && tick < 30000) @(negedge ap_clk);
    if (completed != 32 || accepted != 32 || writes != 8192 || reads != 8192 || overlaps == 0)
      $fatal(1, "Queued counts accepted=%0d completed=%0d writes=%0d reads=%0d overlap=%0d",
             accepted, completed, writes, reads, overlaps);
    repeat (5) @(negedge ap_clk);
    if (!ap_idle || ap_done || writes != 8192 || reads != 8192)
      $fatal(1, "Effects after queue drain");
    for (index = 0; index < 32; index = index + 1)
      $display("QUEUE_EVENT trial=%0d accepted=%0d last_commit=%0d done=%0d",
               index, accepts[index], commits[index], finishes[index]);
    $display("QUEUED PRESSURE TESTS PASSED: 32 calls, 8192 writes, overlaps=%0d", overlaps);
    $finish;
  end
endmodule
