`timescale 1ns/1ps

module backend_pipeline_recurrence_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done;
  wire ap_idle;
  wire ap_ready;
  wire [31:0] ap_return;
  wire [3:0] input_address0;
  wire input_ce0;
  reg [31:0] input_q0 = 32'd0;

  reg [31:0] input_memory [0:15];
  integer index;
  integer cycles;
  integer expected;

  backend_pipeline_recurrence dut (
    .ap_clk(ap_clk),
    .ap_rst(ap_rst),
    .ap_start(ap_start),
    .ap_done(ap_done),
    .ap_idle(ap_idle),
    .ap_ready(ap_ready),
    .ap_return(ap_return),
    .input_r_address0(input_address0),
    .input_r_ce0(input_ce0),
    .input_r_q0(input_q0)
  );

  always #5 ap_clk = ~ap_clk;

  always @(posedge ap_clk) begin
    if (input_ce0)
      input_q0 <= input_memory[input_address0];
  end

  initial begin
    expected = 0;
    for (index = 0; index < 16; index = index + 1) begin
      input_memory[index] = index + 3;
      expected = expected + index + 3;
    end
    cycles = 0;

    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    @(negedge ap_clk);
    ap_start = 1'b1;

    while (!ap_done && cycles < 500) begin
      @(negedge ap_clk);
      cycles = cycles + 1;
    end
    if (!ap_done) begin
      $display("FAIL: timeout waiting for ap_done");
      $fatal(1);
    end
    ap_start = 1'b0;
    if (ap_return !== expected) begin
      $display("FAIL: expected sum=%0d actual=%0d", expected, ap_return);
      $fatal(1);
    end
    $display("RESULT %0d", ap_return);
    $display("ALL PIPELINE RECURRENCE TESTS PASSED cycles=%0d", cycles);
    $finish;
  end
endmodule
