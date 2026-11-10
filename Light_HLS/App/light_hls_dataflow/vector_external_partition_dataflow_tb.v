`timescale 1ns/1ps

module vector_external_partition_dataflow_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done, ap_idle, ap_ready;
  wire [3:0] input_r_0_address0, input_r_1_address0;
  wire [3:0] input_r_2_address0, input_r_3_address0;
  wire input_r_0_ce0, input_r_1_ce0, input_r_2_ce0, input_r_3_ce0;
  reg [31:0] input_r_0_q0 = 0, input_r_1_q0 = 0;
  reg [31:0] input_r_2_q0 = 0, input_r_3_q0 = 0;
  wire [5:0] output_r_address0;
  wire output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] bank0 [0:15];
  reg [31:0] bank1 [0:15];
  reg [31:0] bank2 [0:15];
  reg [31:0] bank3 [0:15];
  reg [31:0] result [0:63];
  integer index, cycles, writes;

  vector_external_partition_dataflow dut (
    .ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
    .input_r_0_address0(input_r_0_address0),
    .input_r_0_ce0(input_r_0_ce0), .input_r_0_q0(input_r_0_q0),
    .input_r_1_address0(input_r_1_address0),
    .input_r_1_ce0(input_r_1_ce0), .input_r_1_q0(input_r_1_q0),
    .input_r_2_address0(input_r_2_address0),
    .input_r_2_ce0(input_r_2_ce0), .input_r_2_q0(input_r_2_q0),
    .input_r_3_address0(input_r_3_address0),
    .input_r_3_ce0(input_r_3_ce0), .input_r_3_q0(input_r_3_q0),
    .output_r_address0(output_r_address0), .output_r_ce0(output_r_ce0),
    .output_r_we0(output_r_we0), .output_r_d0(output_r_d0)
  );

  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    if (input_r_0_ce0) input_r_0_q0 <= bank0[input_r_0_address0];
    if (input_r_1_ce0) input_r_1_q0 <= bank1[input_r_1_address0];
    if (input_r_2_ce0) input_r_2_q0 <= bank2[input_r_2_address0];
    if (input_r_3_ce0) input_r_3_q0 <= bank3[input_r_3_address0];
    if (output_r_ce0 && output_r_we0) begin
      result[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end
  end

  initial begin
    for (index = 0; index < 16; index = index + 1) begin
      bank0[index] = (index * 4) * 9 + 2;
      bank1[index] = (index * 4 + 1) * 9 + 2;
      bank2[index] = (index * 4 + 2) * 9 + 2;
      bank3[index] = (index * 4 + 3) * 9 + 2;
    end
    for (index = 0; index < 64; index = index + 1)
      result[index] = 32'hdeadbeef;
    cycles = 0;
    writes = 0;
    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    @(negedge ap_clk);
    ap_start = 1'b1;
    @(negedge ap_clk);
    ap_start = 1'b0;
    while (!ap_done && cycles < 2000) begin
      @(negedge ap_clk);
      cycles = cycles + 1;
    end
    if (!ap_done || writes != 64)
      $fatal(1, "FAIL: partitioned DATAFLOW completion writes=%0d", writes);
    for (index = 0; index < 64; index = index + 1) begin
      if (result[index] !== index * 9 + 7)
        $fatal(1, "FAIL: partitioned output[%0d]=%0d", index, result[index]);
    end
    $display("TESTS PASSED: external partition DATAFLOW cycles=%0d", cycles);
    $finish;
  end
endmodule
