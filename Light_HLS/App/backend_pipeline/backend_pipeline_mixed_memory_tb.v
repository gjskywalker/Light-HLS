`timescale 1ns/1ps

module backend_pipeline_mixed_memory_tb;
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
  integer write_count;

  backend_pipeline_mixed_memory dut (
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
      if (memory_we0) begin
        storage[memory_address0] <= memory_d0;
        write_count = write_count + 1;
      end else begin
        memory_q0 <= storage[memory_address0];
      end
    end
  end

  initial begin
    for (index = 0; index < 16; index = index + 1)
      storage[index] = index * 11 + 6;
    cycles = 0;
    write_count = 0;

    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    @(negedge ap_clk);
    ap_start = 1'b1;

    while (!ap_done && cycles < 300) begin
      @(negedge ap_clk);
      cycles = cycles + 1;
    end
    if (!ap_done) begin
      $display("FAIL: timeout waiting for ap_done");
      $fatal(1);
    end
    ap_start = 1'b0;
    @(posedge ap_clk);
    #1;
    if (write_count != 8) begin
      $display("FAIL: expected 8 writes, actual=%0d", write_count);
      $fatal(1);
    end
    for (index = 0; index < 8; index = index + 1) begin
      if (storage[index + 8] !== storage[index] + 1) begin
        $display("FAIL: memory[%0d] expected=%0d actual=%0d", index + 8,
                 storage[index] + 1, storage[index + 8]);
        $fatal(1);
      end
      $display("RESULT %0d %0d", index + 8, storage[index + 8]);
    end
    $display("ALL MIXED-MEMORY PIPELINE TESTS PASSED cycles=%0d", cycles);
    $finish;
  end
endmodule
