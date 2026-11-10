`timescale 1ns/1ps

module backend_scalar_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  reg [31:0] a = 32'd0;
  reg [31:0] b = 32'd0;
  reg [31:0] mode = 32'd0;
  wire ap_done;
  wire ap_idle;
  wire ap_ready;
  wire [31:0] ap_return;

  backend_scalar dut (
    .ap_clk(ap_clk),
    .ap_rst(ap_rst),
    .ap_start(ap_start),
    .ap_done(ap_done),
    .ap_idle(ap_idle),
    .ap_ready(ap_ready),
    .a(a),
    .b(b),
    .mode(mode),
    .ap_return(ap_return)
  );

  always #5 ap_clk = ~ap_clk;

  function automatic [31:0] reference_result;
    input [31:0] input_a;
    input [31:0] input_b;
    input [31:0] input_mode;
    integer index;
    reg [31:0] accumulator;
    begin
      accumulator = input_mode[0] ? input_a : input_b;
      for (index = 0; index < 4; index = index + 1)
        accumulator = accumulator * 3 + (input_a ^ index);
      if (input_mode[1])
        accumulator = accumulator - input_b;
      else
        accumulator = accumulator + input_b;
      reference_result = accumulator;
    end
  endfunction

  task automatic run_case;
    input [31:0] input_a;
    input [31:0] input_b;
    input [31:0] input_mode;
    reg [31:0] expected;
    integer cycles;
    begin
      expected = reference_result(input_a, input_b, input_mode);
      @(negedge ap_clk);
      a = input_a;
      b = input_b;
      mode = input_mode;
      ap_start = 1'b1;
      @(negedge ap_clk);
      ap_start = 1'b0;

      cycles = 0;
      while (!ap_done && cycles < 200) begin
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (!ap_done) begin
        $display("FAIL: timeout for a=%0d b=%0d mode=%0d", input_a,
                 input_b, input_mode);
        $fatal(1);
      end
      if (ap_return !== expected) begin
        $display("FAIL: a=%0d b=%0d mode=%0d expected=%0d actual=%0d",
                 input_a, input_b, input_mode, expected, ap_return);
        $fatal(1);
      end
      $display("RESULT %0d %0d %0d %0d", input_a, input_b,
               input_mode, ap_return);
      $display("PASS: a=%0d b=%0d mode=%0d result=%0d cycles=%0d",
               input_a, input_b, input_mode, ap_return, cycles);
    end
  endtask

  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    run_case(32'd5, 32'd2, 32'd0);
    run_case(32'd7, 32'd3, 32'd1);
    run_case(32'd9, 32'd4, 32'd2);
    run_case(32'd11, 32'd6, 32'd3);
    $display("ALL BACKEND SCALAR TESTS PASSED");
    $finish;
  end
endmodule
