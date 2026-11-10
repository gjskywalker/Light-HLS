`timescale 1ns/1ps

module backend_memory_depth_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done;
  wire ap_idle;
  wire ap_ready;
  wire [3:0] memory_address0;
  wire memory_ce0;
  wire memory_we0;
  wire [31:0] memory_d0;
  reg [31:0] memory_q0 = 32'd0;

  reg [31:0] storage [0:15];
  integer index;
  integer cycles;

  backend_memory_depth dut (
    .ap_clk(ap_clk),
    .ap_rst(ap_rst),
    .ap_start(ap_start),
    .ap_done(ap_done),
    .ap_idle(ap_idle),
    .ap_ready(ap_ready),
    .memory_address0(memory_address0),
    .memory_ce0(memory_ce0),
    .memory_we0(memory_we0),
    .memory_d0(memory_d0),
    .memory_q0(memory_q0)
  );

  always #5 ap_clk = ~ap_clk;

  always @(posedge ap_clk) begin
    if (memory_ce0) begin
      if (memory_we0)
        storage[memory_address0] <= memory_d0;
      else
        memory_q0 <= storage[memory_address0];
    end
  end

  initial begin
    for (index = 0; index < 16; index = index + 1)
      storage[index] = 100 + index;

    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    @(negedge ap_clk);
    ap_start = 1'b1;
    @(negedge ap_clk);
    ap_start = 1'b0;

    cycles = 0;
    while (!ap_done && cycles < 200) begin
      @(negedge ap_clk);
      cycles = cycles + 1;
    end
    if (!ap_done) begin
      $display("FAIL: timeout waiting for ap_done");
      $fatal(1);
    end

    for (index = 0; index < 4; index = index + 1) begin
      if (storage[index] !== 101 + 2 * index) begin
        $display("FAIL: storage[%0d] expected=%0d actual=%0d", index,
                 101 + 2 * index, storage[index]);
        $fatal(1);
      end
    end
    $display("ALL CONFIGURED MEMORY DEPTH TESTS PASSED cycles=%0d", cycles);
    $finish;
  end
endmodule
