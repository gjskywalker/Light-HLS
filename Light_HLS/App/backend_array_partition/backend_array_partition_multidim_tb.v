`timescale 1ns/1ps

module backend_array_partition_multidim_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] input_0 = 3, input_1 = 5, input_2 = 7, input_3 = 11;
  reg [31:0] input_4 = 13, input_5 = 17, input_6 = 19, input_7 = 23;
  wire output_r_address0, output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] output_memory [0:1];
  integer cycles, writes;

  backend_array_partition_multidim dut (
    .ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
`ifdef VITIS_MULTIDIM_PORTS
    .input_0_0(input_0), .input_0_1(input_1), .input_0_2(input_2),
    .input_0_3(input_3), .input_1_0(input_4), .input_1_1(input_5),
    .input_1_2(input_6), .input_1_3(input_7),
`else
    .input_0(input_0), .input_1(input_1), .input_2(input_2),
    .input_3(input_3), .input_4(input_4), .input_5(input_5),
    .input_6(input_6), .input_7(input_7),
`endif
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
    if (!ap_done) $fatal(1, "FAIL: multidimensional timeout");
    // Some controllers announce done before the final write edge. Keep start
    // until that commit, but never across an additional acceptance edge.
    if (writes < 2) begin
      @(posedge ap_clk);
      #1;
    end
    ap_start = 1'b0;
    @(posedge ap_clk);
    #1;
    repeat (2) @(posedge ap_clk);
    #1;
    if (writes != 2) $fatal(1, "FAIL: multidimensional writes=%0d", writes);
    if (output_memory[0] !== input_0 + input_1 + input_2 + input_3)
      $fatal(1, "FAIL: multidimensional output[0]=%0d", output_memory[0]);
    if (output_memory[1] !== input_4 + input_5 + input_6 + input_7)
      $fatal(1, "FAIL: multidimensional output[1]=%0d", output_memory[1]);
    $display("RESULT output 0 %0d", output_memory[0]);
    $display("RESULT output 1 %0d", output_memory[1]);
    $display("TESTS PASSED: multidimensional dim=0 partition cycles=%0d",
             cycles);
    $finish;
  end
endmodule
