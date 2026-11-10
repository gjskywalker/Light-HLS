`timescale 1ns/1ps

module public_fifo_dataflow_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  wire input_stream_read;
  wire output_stream_write;
  wire [31:0] output_stream_din;
  wire [31:0] input_stream_dout;
  wire input_stream_empty_n;
  reg output_stream_full_n = 1;
  reg [31:0] input_memory [0:15];
  reg [31:0] result [0:15];
  integer input_index, output_index, cycles, index;

  assign input_stream_empty_n = input_index < 16;
  assign input_stream_dout = input_memory[input_index];

  public_fifo_dataflow dut (
    .ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
    .input_stream_dout(input_stream_dout),
    .input_stream_empty_n(input_stream_empty_n),
    .input_stream_read(input_stream_read),
    .output_stream_din(output_stream_din),
    .output_stream_full_n(output_stream_full_n),
    .output_stream_write(output_stream_write)
  );

  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    if (input_stream_read && input_stream_empty_n)
      input_index <= input_index + 1;
    if (output_stream_write && output_stream_full_n) begin
      result[output_index] = output_stream_din;
      output_index = output_index + 1;
    end
    if (!ap_rst)
      output_stream_full_n <= (cycles % 4) != 1;
  end

  initial begin
    for (index = 0; index < 16; index = index + 1) begin
      input_memory[index] = index * 6 + 2;
      result[index] = 0;
    end
    input_index = 0; output_index = 0; cycles = 0;
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    @(negedge ap_clk); ap_start = 1;
    @(negedge ap_clk); ap_start = 0;
    while (!ap_done && cycles < 1000) begin
      @(negedge ap_clk); cycles = cycles + 1;
    end
    if (!ap_done || input_index != 16 || output_index != 16)
      $fatal(1, "FAIL: public FIFO counts input=%0d output=%0d",
             input_index, output_index);
    for (index = 0; index < 16; index = index + 1)
      if (result[index] !== (index * 6 + 2) * 2 + 4)
        $fatal(1, "FAIL: public FIFO output[%0d]=%0d", index, result[index]);
    $display("TESTS PASSED: public FIFO DATAFLOW cycles=%0d", cycles);
    $finish;
  end
endmodule
