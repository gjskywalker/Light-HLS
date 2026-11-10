`timescale 1ns/1ps

module backend_array_partition_block_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done;
  wire ap_idle;
  wire ap_ready;
  wire [1:0] input_0_address0, input_1_address0;
  wire [1:0] input_2_address0, input_3_address0;
  wire input_0_ce0, input_1_ce0, input_2_ce0, input_3_ce0;
  reg [31:0] input_0_q0 = 0, input_1_q0 = 0;
  reg [31:0] input_2_q0 = 0, input_3_q0 = 0;
  wire [1:0] output_r_address0;
  wire output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] bank_0 [0:3], bank_1 [0:3];
  reg [31:0] bank_2 [0:3], bank_3 [0:3];
  reg [31:0] logical_input [0:15], output_memory [0:3];
  integer index, cycles, writes;

  backend_array_partition_block dut (
    .ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
    .input_0_address0(input_0_address0), .input_0_ce0(input_0_ce0),
    .input_0_q0(input_0_q0),
    .input_1_address0(input_1_address0), .input_1_ce0(input_1_ce0),
    .input_1_q0(input_1_q0),
    .input_2_address0(input_2_address0), .input_2_ce0(input_2_ce0),
    .input_2_q0(input_2_q0),
    .input_3_address0(input_3_address0), .input_3_ce0(input_3_ce0),
    .input_3_q0(input_3_q0),
    .output_r_address0(output_r_address0), .output_r_ce0(output_r_ce0),
    .output_r_we0(output_r_we0), .output_r_d0(output_r_d0)
  );

  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    if (input_0_ce0) input_0_q0 <= bank_0[input_0_address0];
    if (input_1_ce0) input_1_q0 <= bank_1[input_1_address0];
    if (input_2_ce0) input_2_q0 <= bank_2[input_2_address0];
    if (input_3_ce0) input_3_q0 <= bank_3[input_3_address0];
    if (output_r_ce0 && output_r_we0) begin
      output_memory[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end
  end

  initial begin
    for (index = 0; index < 16; index = index + 1)
      logical_input[index] = index * 5 + 7;
    for (index = 0; index < 4; index = index + 1) begin
      bank_0[index] = logical_input[index];
      bank_1[index] = logical_input[index + 4];
      bank_2[index] = logical_input[index + 8];
      bank_3[index] = logical_input[index + 12];
      output_memory[index] = 32'hdeadbeef;
    end
    cycles = 0;
    writes = 0;
    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    @(negedge ap_clk);
    ap_start = 1'b1;
    while (!ap_done && cycles < 200) begin
      @(negedge ap_clk);
      cycles = cycles + 1;
    end
    if (!ap_done) $fatal(1, "FAIL: block timeout");
    ap_start = 1'b0;
    @(posedge ap_clk);
    #1;
    if (writes != 4) $fatal(1, "FAIL: block write count=%0d", writes);
    for (index = 0; index < 4; index = index + 1)
      if (output_memory[index] !== logical_input[index] +
          logical_input[index + 4] + logical_input[index + 8] +
          logical_input[index + 12])
        $fatal(1, "FAIL: block output[%0d]=%0d", index,
               output_memory[index]);
    for (index = 0; index < 4; index = index + 1)
      $display("RESULT output %0d %0d", index, output_memory[index]);
    $display("TESTS PASSED: block array partition cycles=%0d", cycles);
    $finish;
  end
endmodule
