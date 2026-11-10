`timescale 1ns/1ps

module vector_stream_array_dataflow_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done, ap_idle, ap_ready;
  wire [2:0] input_r_address0;
  wire input_r_ce0;
  reg [31:0] input_r_q0 = 0;
  wire [2:0] output_r_address0;
  wire output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] input_memory [0:5];
  reg [31:0] output_memory [0:5];
  integer index, cycles, writes;

  vector_stream_array_dataflow dut (
    .ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
    .input_r_address0(input_r_address0), .input_r_ce0(input_r_ce0),
    .input_r_q0(input_r_q0),
    .output_r_address0(output_r_address0), .output_r_ce0(output_r_ce0),
    .output_r_we0(output_r_we0), .output_r_d0(output_r_d0)
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
    for (index = 0; index < 6; index = index + 1) begin
      input_memory[index] = index * 11 + 5;
      output_memory[index] = 32'hdeadbeef;
    end
    cycles = 0;
    writes = 0;
    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    @(negedge ap_clk);
    ap_start = 1'b1;
    @(negedge ap_clk);
    ap_start = 1'b0;
    while (!ap_done && cycles < 1000) begin
      @(negedge ap_clk);
      cycles = cycles + 1;
    end
    if (!ap_done) $fatal(1, "FAIL: stream-array DATAFLOW timeout");
    @(posedge ap_clk);
    #1;
    if (writes != 6)
      $fatal(1, "FAIL: stream-array DATAFLOW writes=%0d", writes);
    for (index = 0; index < 6; index = index + 1) begin
      if (output_memory[index] !== input_memory[index])
        $fatal(1, "FAIL: stream-array output[%0d]=%0d", index,
               output_memory[index]);
      $display("RESULT output %0d %0d", index, output_memory[index]);
    end
    $display("TESTS PASSED: stream-array DATAFLOW cycles=%0d", cycles);
    $finish;
  end
endmodule
