`timescale 1ns/1ps

module backend_array_partition_complete_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] input_0 = 32'd11;
  reg [31:0] input_1 = 32'd23;
  reg [31:0] input_2 = 32'd37;
  reg [31:0] input_3 = 32'd41;
  wire [1:0] output_r_address0;
  wire output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] output_memory [0:3];
  integer cycles, writes;

  backend_array_partition_complete dut (
`ifndef VITIS_NO_CLOCK
    .ap_clk(ap_clk),
`endif
    .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
    .input_0(input_0), .input_1(input_1),
    .input_2(input_2), .input_3(input_3),
    .output_r_address0(output_r_address0), .output_r_ce0(output_r_ce0),
    .output_r_we0(output_r_we0), .output_r_d0(output_r_d0)
  );

  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk)
    if (output_r_ce0 && output_r_we0) begin
      output_memory[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end

  initial begin
    output_memory[0] = 32'hdeadbeef;
    cycles = 0;
    writes = 0;
    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    @(negedge ap_clk);
    ap_start = 1'b1;
    while (!ap_done && cycles < 100) begin
      @(negedge ap_clk);
      cycles = cycles + 1;
    end
    if (!ap_done) $fatal(1, "FAIL: complete timeout");
    @(posedge ap_clk);
    #1;
    ap_start = 1'b0;
    @(negedge ap_clk);
    #1;
    if (writes != 1) $fatal(1, "FAIL: complete write count=%0d", writes);
    if (output_memory[0] !== input_0 + input_1 + input_2 + input_3)
      $fatal(1, "FAIL: complete output=%0d", output_memory[0]);
    $display("RESULT output 0 %0d", output_memory[0]);
    $display("TESTS PASSED: complete array partition cycles=%0d", cycles);
    $finish;
  end
endmodule
