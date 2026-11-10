`timescale 1ns/1ps

module backend_array_partition_local_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done, ap_idle, ap_ready;
  wire [3:0] input_r_address0;
  wire input_r_ce0;
  reg [31:0] input_r_q0 = 0;
  wire [1:0] output_r_address0;
  wire output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] input_memory [0:15], output_memory [0:3];
  integer index, cycles, writes;

  backend_array_partition_local dut (
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
    for (index = 0; index < 16; index = index + 1)
      input_memory[index] = index * 3 + 2;
    for (index = 0; index < 4; index = index + 1)
      output_memory[index] = 32'hdeadbeef;
    cycles = 0;
    writes = 0;
    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    @(negedge ap_clk);
    ap_start = 1'b1;
    while (!ap_done && cycles < 400) begin
      @(negedge ap_clk);
      cycles = cycles + 1;
    end
    if (!ap_done) $fatal(1, "FAIL: local partition timeout");
    ap_start = 1'b0;
    @(posedge ap_clk);
    #1;
    if (writes != 4) $fatal(1, "FAIL: local write count=%0d", writes);
    for (index = 0; index < 4; index = index + 1)
      if (output_memory[index] !== input_memory[4 * index] +
          input_memory[4 * index + 1] + input_memory[4 * index + 2] +
          input_memory[4 * index + 3] + 4)
        $fatal(1, "FAIL: local output[%0d]=%0d", index,
               output_memory[index]);
    for (index = 0; index < 4; index = index + 1)
      $display("RESULT output %0d %0d", index, output_memory[index]);
    $display("TESTS PASSED: local array partition cycles=%0d", cycles);
    $finish;
  end
endmodule
