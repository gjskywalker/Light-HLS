`timescale 1ns/1ps

module vector_pipeline_partition_dataflow_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  wire [3:0] input_r_address0, output_r_address0;
  wire input_r_ce0, output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] input_r_q0 = 0;
`ifdef DATAFLOW_VITIS_DUAL_PORT
  wire [3:0] input_r_address1, output_r_address1;
  wire input_r_ce1, output_r_ce1, output_r_we1;
  wire [31:0] output_r_d1;
  reg [31:0] input_r_q1 = 0;
`endif
  reg [31:0] input_memory [0:15];
  reg [31:0] output_memory [0:15];
  integer index, cycles, writes;

  vector_pipeline_partition_dataflow dut (
    .ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
    .input_r_address0(input_r_address0), .input_r_ce0(input_r_ce0),
    .input_r_q0(input_r_q0), .output_r_address0(output_r_address0),
    .output_r_ce0(output_r_ce0), .output_r_we0(output_r_we0),
    .output_r_d0(output_r_d0)
`ifdef DATAFLOW_VITIS_DUAL_PORT
    , .input_r_address1(input_r_address1), .input_r_ce1(input_r_ce1),
    .input_r_q1(input_r_q1), .output_r_address1(output_r_address1),
    .output_r_ce1(output_r_ce1), .output_r_we1(output_r_we1),
    .output_r_d1(output_r_d1)
`endif
  );

  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    if (input_r_ce0) input_r_q0 <= input_memory[input_r_address0];
`ifdef DATAFLOW_VITIS_DUAL_PORT
    if (input_r_ce1) input_r_q1 <= input_memory[input_r_address1];
`endif
    if (output_r_ce0 && output_r_we0) begin
      output_memory[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end
`ifdef DATAFLOW_VITIS_DUAL_PORT
    if (output_r_ce1 && output_r_we1) begin
      output_memory[output_r_address1] <= output_r_d1;
      writes = writes + 1;
    end
`endif
  end

  initial begin
    for (index = 0; index < 16; index = index + 1) begin
      input_memory[index] = index * 4 + 1;
      output_memory[index] = 0;
    end
    cycles = 0; writes = 0;
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    @(negedge ap_clk); ap_start = 1;
    begin : wait_for_acceptance
      @(posedge ap_clk); #1; cycles = cycles + 1;
      while (!ap_ready && cycles < 1000) begin
        @(posedge ap_clk); #1; cycles = cycles + 1;
      end
    end
    if (!ap_ready)
      $fatal(1, "FAIL: combined DATAFLOW input handshake timeout");
`ifndef DATAFLOW_VITIS_HOLD_START_UNTIL_DONE
    @(negedge ap_clk); ap_start = 0;
`endif
    while (!ap_done && cycles < 1000) begin
      @(negedge ap_clk); cycles = cycles + 1;
    end
    if (!ap_done)
      $fatal(1, "FAIL: combined DATAFLOW timeout");
`ifdef DATAFLOW_VITIS_HOLD_START_UNTIL_DONE
    ap_start = 0;
`endif
    @(posedge ap_clk); #1;
    if (writes != 16)
      $fatal(1, "FAIL: combined DATAFLOW writes=%0d", writes);
    for (index = 0; index < 16; index = index + 1) begin
      if (output_memory[index] !== (index * 4 + 3) * 3)
        $fatal(1, "FAIL: combined output[%0d]=%0d", index,
               output_memory[index]);
      $display("RESULT output %0d %0d", index, output_memory[index]);
    end
    $display("TESTS PASSED: pipeline partition DATAFLOW cycles=%0d", cycles);
    $finish;
  end
endmodule
