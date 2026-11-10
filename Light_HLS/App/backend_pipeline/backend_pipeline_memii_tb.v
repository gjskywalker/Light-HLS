`timescale 1ns/1ps

module backend_pipeline_memii_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done;
  wire ap_idle;
  wire ap_ready;
  wire [4:0] input_address0;
  wire input_ce0;
  reg [31:0] input_q0 = 32'd0;
  wire [3:0] output_address0;
  wire output_ce0;
  wire output_we0;
  wire [31:0] output_d0;

  reg [31:0] input_memory [0:31];
  reg [31:0] output_memory [0:15];
  integer index;
  integer cycles;
  integer write_count;
  integer last_write_cycle;

  backend_pipeline_memii dut (
    .ap_clk(ap_clk),
    .ap_rst(ap_rst),
    .ap_start(ap_start),
    .ap_done(ap_done),
    .ap_idle(ap_idle),
    .ap_ready(ap_ready),
    .input_r_address0(input_address0),
    .input_r_ce0(input_ce0),
    .input_r_q0(input_q0),
    .output_r_address0(output_address0),
    .output_r_ce0(output_ce0),
    .output_r_we0(output_we0),
    .output_r_d0(output_d0)
  );

  always #5 ap_clk = ~ap_clk;

  always @(posedge ap_clk) begin
    if (input_ce0)
      input_q0 <= input_memory[input_address0];
    if (output_ce0) begin
      if (output_we0) begin
        output_memory[output_address0] <= output_d0;
        if (write_count > 0 && cycles != last_write_cycle + 2) begin
          $display("FAIL: output writes are not separated by achieved II=2");
          $fatal(1);
        end
        last_write_cycle = cycles;
        write_count = write_count + 1;
      end
    end
  end

  initial begin
    for (index = 0; index < 32; index = index + 1)
      input_memory[index] = index * 5 + 1;
    for (index = 0; index < 16; index = index + 1)
      output_memory[index] = 32'hdeadbeef;
    cycles = 0;
    write_count = 0;
    last_write_cycle = -1;

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
    if (write_count != 16) begin
      $display("FAIL: expected 16 output writes, actual=%0d", write_count);
      $fatal(1);
    end
    for (index = 0; index < 16; index = index + 1) begin
      if (output_memory[index] !==
          input_memory[index] + input_memory[index + 16]) begin
        $display("FAIL: output[%0d] expected=%0d actual=%0d", index,
                 input_memory[index] + input_memory[index + 16],
                 output_memory[index]);
        $fatal(1);
      end
      $display("RESULT %0d %0d", index, output_memory[index]);
    end
    $display("ALL MEMORY-LIMITED PIPELINE TESTS PASSED cycles=%0d", cycles);
    $finish;
  end
endmodule
