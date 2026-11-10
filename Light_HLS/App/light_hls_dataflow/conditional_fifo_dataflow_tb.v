`timescale 1ns/1ps

module conditional_fifo_dataflow_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  reg [31:0] enabled = 1;
  wire ap_done, ap_idle, ap_ready;
  wire output_r_address0, output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  integer cycles, trial, writes, pushes, pops;
  reg [31:0] result = 0;

  conditional_fifo_dataflow dut (
    .ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
    .enabled(enabled), .output_r_address0(output_r_address0),
    .output_r_ce0(output_r_ce0), .output_r_we0(output_r_we0),
    .output_r_d0(output_r_d0)
  );

  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    if (!ap_rst && output_r_ce0 && output_r_we0) begin
      result <= output_r_d0;
      writes = writes + 1;
    end
    if (!ap_rst && dut.channel_values_0_write && !dut.channel_values_0_full)
      pushes = pushes + 1;
    if (!ap_rst && dut.channel_values_0_read && !dut.channel_values_0_empty)
      pops = pops + 1;
  end

  initial begin
    cycles = 0;
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    for (trial = 0; trial < 64; trial = trial + 1) begin
      enabled = trial % 2;
      cycles = 0;
      writes = 0;
      pushes = 0;
      pops = 0;
      result = 32'hdeadbeef;
      @(negedge ap_clk); ap_start = 1;
      @(negedge ap_clk); ap_start = 0;
      while (!ap_done && cycles < 100) begin
        @(negedge ap_clk); cycles = cycles + 1;
      end
      if (!ap_done || result !== (enabled ? 42 : 0) ||
          writes != 1 || pushes != enabled || pops != enabled)
        $fatal(1, "FAIL: conditional FIFO enabled=%0d result=%0d writes=%0d tokens=%0d/%0d",
               enabled, result, writes, pushes, pops);
      repeat (3) @(negedge ap_clk);
    end
    $display("TESTS PASSED: conditional FIFO DATAFLOW 64 alternating transactions");
    $finish;
  end
endmodule
