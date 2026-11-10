`timescale 1ns/1ps

module vector_partial_pipo_dataflow_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  wire [1:0] input_r_address0, output_r_address0;
  wire input_r_ce0, output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] input_r_q0 = 0;
  reg [31:0] input_memory [0:2];
  reg [31:0] output_memory [0:2];
  integer index, cycles, writes;

  vector_partial_pipo_dataflow dut (
    .ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
    .input_r_address0(input_r_address0), .input_r_ce0(input_r_ce0),
    .input_r_q0(input_r_q0), .output_r_address0(output_r_address0),
    .output_r_ce0(output_r_ce0), .output_r_we0(output_r_we0),
    .output_r_d0(output_r_d0)
  );

  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    if (input_r_ce0) input_r_q0 <= input_memory[input_r_address0];
    if (output_r_ce0 && output_r_we0) begin
      output_memory[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end
  end

  initial begin
    for (index = 0; index < 3; index = index + 1) begin
      input_memory[index] = index * 7 + 1;
      output_memory[index] = 0;
    end
    cycles = 0;
    writes = 0;
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    @(negedge ap_clk); ap_start = 1;
    @(negedge ap_clk); ap_start = 0;
    while (!ap_done && cycles < 500) begin
      @(negedge ap_clk); cycles = cycles + 1;
    end
    if (!ap_done || writes != 3) $fatal(1, "FAIL: partial PIPO completion");
    for (index = 0; index < 3; index = index + 1)
      if (output_memory[index] !== (index * 7 + 5) * 2)
        $fatal(1, "FAIL: partial PIPO output[%0d]=%0d", index,
               output_memory[index]);
    $display("TESTS PASSED: partial PIPO DATAFLOW cycles=%0d", cycles);
    $finish;
  end
endmodule
