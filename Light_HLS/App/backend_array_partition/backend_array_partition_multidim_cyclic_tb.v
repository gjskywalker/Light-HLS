`timescale 1ns/1ps

module backend_array_partition_multidim_cyclic_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done, ap_idle, ap_ready;
  wire [1:0] input_0_address0, input_1_address0;
  wire input_0_ce0, input_1_ce0;
  reg [31:0] input_0_q0 = 0, input_1_q0 = 0;
  wire output_r_address0, output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] input_bank_0 [0:3], input_bank_1 [0:3];
  reg [31:0] output_memory [0:1];
  integer cycles, writes;

  backend_array_partition_multidim_cyclic dut (
    .ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
    .input_0_address0(input_0_address0), .input_0_ce0(input_0_ce0),
    .input_0_q0(input_0_q0),
    .input_1_address0(input_1_address0), .input_1_ce0(input_1_ce0),
    .input_1_q0(input_1_q0),
    .output_r_address0(output_r_address0), .output_r_ce0(output_r_ce0),
    .output_r_we0(output_r_we0), .output_r_d0(output_r_d0)
  );

  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    if (input_0_ce0) input_0_q0 <= input_bank_0[input_0_address0];
    if (input_1_ce0) input_1_q0 <= input_bank_1[input_1_address0];
    if (output_r_ce0 && output_r_we0) begin
      output_memory[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end
  end

  initial begin
    input_bank_0[0] = 3;
    input_bank_0[1] = 7;
    input_bank_0[2] = 13;
    input_bank_0[3] = 19;
    input_bank_1[0] = 5;
    input_bank_1[1] = 11;
    input_bank_1[2] = 17;
    input_bank_1[3] = 23;
    output_memory[0] = 32'hdeadbeef;
    output_memory[1] = 32'hdeadbeef;
    cycles = 0;
    writes = 0;
    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    @(negedge ap_clk);
    ap_start = 1'b1;
    while (!ap_done && cycles < 150) begin
      @(negedge ap_clk);
      cycles = cycles + 1;
    end
    if (!ap_done) $fatal(1, "FAIL: multidimensional cyclic timeout");
    ap_start = 1'b0;
    @(posedge ap_clk);
    #1;
    if (writes != 2)
      $fatal(1, "FAIL: multidimensional cyclic writes=%0d", writes);
    if (output_memory[0] !== 3 + 5 + 7 + 11)
      $fatal(1, "FAIL: multidimensional cyclic output[0]=%0d",
             output_memory[0]);
    if (output_memory[1] !== 13 + 17 + 19 + 23)
      $fatal(1, "FAIL: multidimensional cyclic output[1]=%0d",
             output_memory[1]);
    $display("RESULT output 0 %0d", output_memory[0]);
    $display("RESULT output 1 %0d", output_memory[1]);
    $display("TESTS PASSED: multidimensional cyclic partition cycles=%0d",
             cycles);
    $finish;
  end
endmodule
