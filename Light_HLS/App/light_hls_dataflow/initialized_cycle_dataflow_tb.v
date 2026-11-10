`timescale 1ns/1ps

module initialized_cycle_dataflow_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  wire output_r_address0, output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  integer cycles, writes;
  reg [31:0] result;

  initialized_cycle_dataflow dut (
    .ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
    .output_r_address0(output_r_address0), .output_r_ce0(output_r_ce0),
    .output_r_we0(output_r_we0), .output_r_d0(output_r_d0)
  );

  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    if (output_r_ce0 && output_r_we0) begin
      result <= output_r_d0;
      writes = writes + 1;
    end
  end

  initial begin
    cycles = 0; writes = 0; result = 32'hdeadbeef;
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    @(negedge ap_clk); ap_start = 1;
    @(negedge ap_clk); ap_start = 0;
    while (!ap_done && cycles < 500) begin
      @(negedge ap_clk); cycles = cycles + 1;
    end
    if (!ap_done || writes != 1 || result !== 1)
      $fatal(1, "FAIL: initialized cycle writes=%0d result=%0d", writes, result);
    $display("TESTS PASSED: initialized cycle DATAFLOW cycles=%0d", cycles);
    $finish;
  end
endmodule
