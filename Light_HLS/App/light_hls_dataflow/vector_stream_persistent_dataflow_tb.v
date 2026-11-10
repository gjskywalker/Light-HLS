`timescale 1ns/1ps

module vector_stream_persistent_dataflow_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  reg [31:0] bias = 0;
  wire ap_done, ap_idle, ap_ready;
  wire [5:0] input_r_address0;
  wire input_r_ce0;
  reg [31:0] input_r_q0 = 0;
  wire [5:0] output_r_address0;
  wire output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] input_memory [0:63];
  reg [31:0] output_memory [0:63];
  integer index, cycles, writes, completions;
  integer expected_bias;

  vector_stream_persistent_dataflow dut (
    .ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
    .input_r_address0(input_r_address0), .input_r_ce0(input_r_ce0),
    .input_r_q0(input_r_q0),
    .output_r_address0(output_r_address0), .output_r_ce0(output_r_ce0),
    .output_r_we0(output_r_we0), .output_r_d0(output_r_d0), .bias(bias)
  );

  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    if (input_r_ce0)
      input_r_q0 <= input_memory[input_r_address0];
    if (output_r_ce0 && output_r_we0) begin
      expected_bias = writes < 64 ? 10 : 100;
      if (output_r_d0 !== input_memory[output_r_address0] * 3 + expected_bias)
        $fatal(1, "FAIL: transaction data mismatch write=%0d address=%0d data=%0d",
               writes, output_r_address0, output_r_d0);
      output_memory[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end
    if (ap_done)
      completions = completions + 1;
  end

  initial begin
    for (index = 0; index < 64; index = index + 1) begin
      input_memory[index] = index * 7 + 3;
      output_memory[index] = 32'hdeadbeef;
    end
    cycles = 0;
    writes = 0;
    completions = 0;
    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    @(negedge ap_clk);
    if (!ap_ready) $fatal(1, "FAIL: controller is not initially ready");
    bias = 10;
    ap_start = 1'b1;
    @(negedge ap_clk);
    if (!ap_ready) $fatal(1, "FAIL: second transaction was not accepted");
    bias = 100;
    @(negedge ap_clk);
    ap_start = 1'b0;
    while ((completions < 2 || writes < 128) && cycles < 3000) begin
      @(negedge ap_clk);
      cycles = cycles + 1;
    end
    if (completions != 2)
      $fatal(1, "FAIL: persistent completions=%0d", completions);
    if (writes != 128)
      $fatal(1, "FAIL: persistent writes=%0d", writes);
    for (index = 0; index < 64; index = index + 1) begin
      if (output_memory[index] !== input_memory[index] * 3 + 100)
        $fatal(1, "FAIL: final output[%0d]=%0d", index,
               output_memory[index]);
    end
    $display("TESTS PASSED: persistent DATAFLOW cycles=%0d completions=%0d",
             cycles, completions);
    $finish;
  end
endmodule
