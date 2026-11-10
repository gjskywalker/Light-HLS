`timescale 1ns/1ps

module signed_chain_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  wire [3:0] input_r_address0, output_r_address0;
  wire input_r_ce0, output_r_ce0, output_r_we0;
  reg [31:0] input_r_q0 = 0;
  wire [31:0] output_r_d0;
  reg [31:0] source [0:15], result [0:15];
  reg signed [15:0] narrow;
  reg signed [31:0] extended, expected;
  integer index, trial, cycles, writes;
  signed_chain dut (.*);
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
    for (trial = 0; trial < 16; trial = trial + 1) begin
      for (index = 0; index < 16; index = index + 1) begin
        source[index] = 32'h12348000 + trial * 7919 + index * 8191;
        result[index] = 32'hdeadbeef;
      end
      writes = 0;
      ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0;
      cycles = 0;
      while (!ap_done && cycles < 200) begin
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (!ap_done || writes != 16)
        $fatal(1, "Signed pipeline timeout or unexpected write count");
      for (index = 0; index < 16; index = index + 1) begin
        narrow = source[index][15:0];
        extended = narrow;
        expected = extended < 0 ? (extended >>> 2) : extended * 3;
        if (result[index] !== expected)
          $fatal(1, "Signed chain: trial=%0d index=%0d expected=%0d actual=%0d",
                 trial, index, expected, result[index]);
      end
      @(negedge ap_clk);
    end
    $display("ALL SIGNED PIPELINE CHAIN TESTS PASSED (256 values)");
    $finish;
  end
endmodule
