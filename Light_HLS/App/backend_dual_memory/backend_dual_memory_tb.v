`timescale 1ns/1ps

module backend_dual_memory_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done;
  wire ap_idle;
  wire ap_ready;
  wire [31:0] ap_return;
  wire [2:0] memory_address0;
  wire memory_ce0;
  reg [31:0] memory_q0 = 32'd0;
  wire [2:0] memory_address1;
  wire memory_ce1;
  reg [31:0] memory_q1 = 32'd0;

  reg [31:0] storage [0:7];
  integer index;
  integer cycles;

  backend_dual_memory dut (
    .ap_clk(ap_clk),
    .ap_rst(ap_rst),
    .ap_start(ap_start),
    .ap_done(ap_done),
    .ap_idle(ap_idle),
    .ap_ready(ap_ready),
    .ap_return(ap_return),
    .memory_address0(memory_address0),
    .memory_ce0(memory_ce0),
    .memory_q0(memory_q0),
    .memory_address1(memory_address1),
    .memory_ce1(memory_ce1),
    .memory_q1(memory_q1)
  );

  always #5 ap_clk = ~ap_clk;

  always @(posedge ap_clk) begin
    if (memory_ce0) begin
      memory_q0 <= storage[memory_address0];
    end
    if (memory_ce1) begin
      memory_q1 <= storage[memory_address1];
    end
  end

  initial begin
    for (index = 0; index < 8; index = index + 1)
      storage[index] = 100 + index;

    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    @(negedge ap_clk);
    ap_start = 1'b1;
    @(negedge ap_clk);
    ap_start = 1'b0;

    cycles = 0;
    while (!ap_done && cycles < 100) begin
      @(negedge ap_clk);
      cycles = cycles + 1;
    end
    if (!ap_done) begin
      $display("FAIL: timeout waiting for ap_done");
      $fatal(1);
    end
    if (ap_return !== 32'd206) begin
      $display("FAIL: expected=206 actual=%0d", ap_return);
      $fatal(1);
    end
    $display("ALL DUAL-PORT AP_MEMORY TESTS PASSED cycles=%0d", cycles);
    $finish;
  end
endmodule
