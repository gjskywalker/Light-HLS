`timescale 1ns/1ps

module backpressure_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  wire [5:0] input_r_address0, output_r_address0;
  wire input_r_ce0, output_r_ce0, output_r_we0;
  reg [31:0] input_r_q0 = 0;
  wire [31:0] output_r_d0;
  reg [31:0] source [0:63], result [0:63], expected;
  integer trial, index, cycles, writes;
  backpressured_pipeline dut (.*);
  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    if (input_r_ce0) input_r_q0 <= source[input_r_address0];
    if (output_r_ce0 && output_r_we0) begin
      result[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end
  end
  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    for (trial = 0; trial < 32; trial = trial + 1) begin
      for (index = 0; index < 64; index = index + 1) begin
        source[index] = index * 7 - trial * 11;
        result[index] = 32'hdeadbeef;
      end
      if (!ap_ready) $fatal(1, "DATAFLOW did not reaccept a transaction");
      writes = 0;
      ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0;
      cycles = 0;
      while (!ap_done && cycles < 4096) begin
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (!ap_done || writes != 64) $fatal(1, "Backpressure timeout or lost writes");
      for (index = 0; index < 64; index = index + 1) begin
        expected = source[index] * 3 + 1;
        if (result[index] !== expected)
          $fatal(1, "Backpressure trial=%0d index=%0d expected=%h actual=%h",
                 trial, index, expected, result[index]);
      end
      repeat (1 + trial % 3) @(negedge ap_clk);
    end
    $display("ALL DEPTH-ONE BACKPRESSURE TESTS PASSED (2048 results)");
    $finish;
  end
endmodule
