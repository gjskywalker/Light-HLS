`timescale 1ns/1ps

module vector_broadcast_dataflow_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  wire [3:0] input_r_address0, left_address0, right_address0;
  wire input_r_ce0, left_ce0, left_we0, right_ce0, right_we0;
  wire [31:0] left_d0, right_d0;
  reg [31:0] input_r_q0 = 0;
  reg [31:0] input_memory [0:15];
  reg [31:0] left_memory [0:15];
  reg [31:0] right_memory [0:15];
  integer index, cycles, left_writes, right_writes;

  vector_broadcast_dataflow dut (
    .ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
    .input_r_address0(input_r_address0), .input_r_ce0(input_r_ce0),
    .input_r_q0(input_r_q0), .left_address0(left_address0),
    .left_ce0(left_ce0), .left_we0(left_we0), .left_d0(left_d0),
    .right_address0(right_address0), .right_ce0(right_ce0),
    .right_we0(right_we0), .right_d0(right_d0)
  );

  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    if (input_r_ce0) input_r_q0 <= input_memory[input_r_address0];
    if (left_ce0 && left_we0) begin
      left_memory[left_address0] <= left_d0;
      left_writes = left_writes + 1;
    end
    if (right_ce0 && right_we0) begin
      right_memory[right_address0] <= right_d0;
      right_writes = right_writes + 1;
    end
  end

  initial begin
    for (index = 0; index < 16; index = index + 1) begin
      input_memory[index] = index * 3 + 2;
      left_memory[index] = 0;
      right_memory[index] = 0;
    end
    cycles = 0; left_writes = 0; right_writes = 0;
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    @(negedge ap_clk); ap_start = 1;
    @(negedge ap_clk); ap_start = 0;
    while (!ap_done && cycles < 1000) begin
      @(negedge ap_clk); cycles = cycles + 1;
    end
    if (!ap_done || left_writes != 16 || right_writes != 16)
      $fatal(1, "FAIL: broadcast completion left=%0d right=%0d",
             left_writes, right_writes);
    for (index = 0; index < 16; index = index + 1) begin
      if (left_memory[index] !== index * 3 + 3)
        $fatal(1, "FAIL: broadcast left[%0d]=%0d", index, left_memory[index]);
      if (right_memory[index] !== (index * 3 + 2) * 2)
        $fatal(1, "FAIL: broadcast right[%0d]=%0d", index, right_memory[index]);
    end
    $display("TESTS PASSED: broadcast DATAFLOW cycles=%0d", cycles);
    $finish;
  end
endmodule
