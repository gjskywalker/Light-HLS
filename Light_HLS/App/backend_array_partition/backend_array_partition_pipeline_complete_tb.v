`timescale 1ns/1ps

module backend_array_partition_pipeline_complete_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] input_0 = 32'd5;
  reg [31:0] input_1 = 32'd11;
  reg [31:0] input_2 = 32'd17;
  reg [31:0] input_3 = 32'd23;
  wire [1:0] output_r_address0;
  wire output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] output_memory [0:3];
  integer index, cycles, writes;

  backend_array_partition_pipeline_complete dut (
    .ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
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

`ifdef TRACE_MEMORY_WRITES
  always @(posedge ap_clk)
    $display("TRACE start=%0d done=%0d ready=%0d ce=%0d we=%0d address=%0d data=%0d",
             ap_start, ap_done, ap_ready, output_r_ce0, output_r_we0,
             output_r_address0, output_r_d0);
`endif

  initial begin
    for (index = 0; index < 4; index = index + 1)
      output_memory[index] = 32'hdeadbeef;
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
    if (!ap_done) $fatal(1, "FAIL: complete pipeline timeout");
    // Some controllers announce done before the final write edge. Keep start
    // until that commit, but never across an additional acceptance edge.
    if (writes < 4) begin
      @(posedge ap_clk);
      #1;
    end
    ap_start = 1'b0;
    @(posedge ap_clk);
    #1;
    repeat (2) @(posedge ap_clk);
    #1;
    if (writes != 4) $fatal(1, "FAIL: complete pipeline writes=%0d", writes);
    if (output_memory[0] !== input_0 * 3 + 1 ||
        output_memory[1] !== input_1 * 3 + 1 ||
        output_memory[2] !== input_2 * 3 + 1 ||
        output_memory[3] !== input_3 * 3 + 1)
      $fatal(1, "FAIL: complete pipeline data mismatch");
    for (index = 0; index < 4; index = index + 1)
      $display("RESULT output %0d %0d", index, output_memory[index]);
    $display("TESTS PASSED: complete partition pipeline cycles=%0d", cycles);
    $finish;
  end
endmodule
