`timescale 1ns/1ps

module backend_pipeline_memory_dependence_tb;
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
  wire [3:0] output_address0;
  wire output_ce0;
  wire output_we0;
  wire [31:0] output_d0;

  reg [31:0] storage [0:15];
  reg [31:0] output_storage [0:14];
  integer index;
  integer cycles;
  integer memory_write_count;
  integer output_write_count;

  backend_pipeline_memory_dependence dut (
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
    .memory_q0(memory_q0),
    .output_r_address0(output_address0),
    .output_r_ce0(output_ce0),
    .output_r_we0(output_we0),
    .output_r_d0(output_d0)
  );

  always #5 ap_clk = ~ap_clk;

  always @(posedge ap_clk) begin
    if (memory_ce0) begin
      if (memory_we0) begin
        storage[memory_address0] <= memory_d0;
        memory_write_count = memory_write_count + 1;
      end else begin
        memory_q0 <= storage[memory_address0];
      end
    end
    if (output_ce0 && output_we0) begin
      output_storage[output_address0] <= output_d0;
      output_write_count = output_write_count + 1;
    end
  end

  initial begin
    for (index = 0; index < 16; index = index + 1)
      storage[index] = index * 13 + 7;
    for (index = 0; index < 15; index = index + 1)
      output_storage[index] = 32'hdeadbeef;
    cycles = 0;
    memory_write_count = 0;
    output_write_count = 0;

    repeat (3) @(negedge ap_clk);
    ap_rst = 1'b0;
    @(negedge ap_clk);
    ap_start = 1'b1;

    while (!ap_done && cycles < 500) begin
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
    if (memory_write_count != 15 || output_write_count != 15) begin
      $display("FAIL: expected 15 writes per memory, memory=%0d output=%0d",
               memory_write_count, output_write_count);
      $fatal(1);
    end
    for (index = 0; index < 15; index = index + 1) begin
      if (storage[index] !== index + 100) begin
        $display("FAIL: memory[%0d] expected=%0d actual=%0d", index,
                 index + 100, storage[index]);
        $fatal(1);
      end
      if (output_storage[index] !== (index + 1) * 13 + 7) begin
        $display("FAIL: output[%0d] expected=%0d actual=%0d", index,
                 (index + 1) * 13 + 7, output_storage[index]);
        $fatal(1);
      end
    end
    $display("RESULT %0d", output_storage[14]);
    $display("ALL MEMORY-DEPENDENCE PIPELINE TESTS PASSED cycles=%0d",
             cycles);
    $finish;
  end
endmodule
