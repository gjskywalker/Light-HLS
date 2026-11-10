`timescale 1ns/1ps

module backend_array_partition_complete_rw_tb;
  reg ap_clk = 1'b0;
  reg ap_rst = 1'b1;
  reg ap_start = 1'b0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] data_0_i = 4, data_1_i = 7;
  reg [31:0] data_2_i = 11, data_3_i = 19;
  wire [31:0] data_0_o, data_1_o, data_2_o, data_3_o;
  wire data_0_o_ap_vld, data_1_o_ap_vld;
  wire data_2_o_ap_vld, data_3_o_ap_vld;
  reg [31:0] results [0:3];
  integer cycles, writes;

  backend_array_partition_complete_rw dut (
    .ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready),
    .data_0_i(data_0_i), .data_0_o(data_0_o),
    .data_0_o_ap_vld(data_0_o_ap_vld),
    .data_1_i(data_1_i), .data_1_o(data_1_o),
    .data_1_o_ap_vld(data_1_o_ap_vld),
    .data_2_i(data_2_i), .data_2_o(data_2_o),
    .data_2_o_ap_vld(data_2_o_ap_vld),
    .data_3_i(data_3_i), .data_3_o(data_3_o),
    .data_3_o_ap_vld(data_3_o_ap_vld)
  );

  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
    if (data_0_o_ap_vld) begin results[0] <= data_0_o; writes = writes + 1; end
    if (data_1_o_ap_vld) begin results[1] <= data_1_o; writes = writes + 1; end
    if (data_2_o_ap_vld) begin results[2] <= data_2_o; writes = writes + 1; end
    if (data_3_o_ap_vld) begin results[3] <= data_3_o; writes = writes + 1; end
  end

  initial begin
    results[0] = 0;
    results[1] = 0;
    results[2] = 0;
    results[3] = 0;
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
    if (!ap_done) $fatal(1, "FAIL: complete read/write timeout");
    ap_start = 1'b0;
    @(posedge ap_clk);
    #1;
    if (writes != 4) $fatal(1, "FAIL: complete read/write count=%0d", writes);
    if (results[0] !== data_0_i * 3 + 1 ||
        results[1] !== data_1_i * 3 + 1 ||
        results[2] !== data_2_i * 3 + 1 ||
        results[3] !== data_3_i * 3 + 1)
      $fatal(1, "FAIL: complete read/write data mismatch");
    $display("RESULT data 0 %0d", results[0]);
    $display("RESULT data 1 %0d", results[1]);
    $display("RESULT data 2 %0d", results[2]);
    $display("RESULT data 3 %0d", results[3]);
    $display("TESTS PASSED: complete read/write partition cycles=%0d", cycles);
    $finish;
  end
endmodule
