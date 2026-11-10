`timescale 1ns/1ps
module scalar_bank_events_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] index, seed;
  reg [31:0] data [0:3], expected [0:3], result [0:3], expected_result [0:3];
  wire [31:0] write_data [0:3], output_data [0:3];
  wire [3:0] data_valid, output_valid;
  integer trial, i, a, b, cycles, writes, output_writes;
  reg [31:0] sampled_index, sampled_seed, old_a, old_b;
  scalar_bank_events dut (
    .ap_clk(ap_clk), .ap_rst(ap_rst), .ap_start(ap_start),
    .ap_done(ap_done), .ap_idle(ap_idle), .ap_ready(ap_ready), .index(index), .seed(seed),
`ifdef READ_ONLY
    .data_0(data[0]), .data_1(data[1]), .data_2(data[2]), .data_3(data[3]),
`else
    .data_0_i(data[0]), .data_1_i(data[1]), .data_2_i(data[2]), .data_3_i(data[3]),
    .data_0_o(write_data[0]), .data_1_o(write_data[1]), .data_2_o(write_data[2]), .data_3_o(write_data[3]),
    .data_0_o_ap_vld(data_valid[0]), .data_1_o_ap_vld(data_valid[1]),
    .data_2_o_ap_vld(data_valid[2]), .data_3_o_ap_vld(data_valid[3]),
`endif
    .output_0(output_data[0]), .output_1(output_data[1]),
    .output_2(output_data[2]), .output_3(output_data[3]),
    .output_0_ap_vld(output_valid[0]), .output_1_ap_vld(output_valid[1]),
    .output_2_ap_vld(output_valid[2]), .output_3_ap_vld(output_valid[3])
  );
  always #5 ap_clk = ~ap_clk;
  integer bank;
  always @(posedge ap_clk) begin
    if (!ap_rst) begin
      if ((^output_valid) === 1'bx) $fatal(1, "Unknown scalar output validity");
      for (bank = 0; bank < 4; bank = bank + 1) begin
        if (`CASE_KIND != 2 && data_valid[bank]) begin
          data[bank] <= write_data[bank];
          writes = writes + 1;
        end
        if (output_valid[bank]) begin
          result[bank] <= output_data[bank];
          output_writes = output_writes + 1;
        end
      end
    end
  end
  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    for (trial = 0; trial < 64; trial = trial + 1) begin
      sampled_index = trial;
      sampled_seed = 32'h81234567 ^ (trial * 32'h31fe129b);
      index = sampled_index;
      seed = sampled_seed;
      for (i = 0; i < 4; i = i + 1) begin
        data[i] = (32'hfb723654 * (i + 1)) ^ trial;
        expected[i] = data[i];
        result[i] = 32'hdeadbeef;
      end
      for (i = 0; i < 4; i = i + 1) begin
        a = (i + sampled_index) & 3;
        b = (a + 1) & 3;
        old_a = expected[a];
        old_b = expected[b];
        if (`CASE_KIND == 2) expected_result[i & 3] = old_a + old_b + sampled_seed;
        else begin
          expected[b] = old_a + sampled_seed + i;
          expected_result[i & 3] = old_b ^ expected[b];
        end
      end
      writes = 0;
      output_writes = 0;
      ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0;
      index = ~sampled_index;
      seed = ~sampled_seed;
      cycles = 0;
      while (!ap_done && cycles < 1000) begin
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (!ap_done || !ap_ready || output_writes != 4 || writes != (`CASE_KIND == 2 ? 0 : 4))
        $fatal(1, "Scalar transaction mismatch trial=%0d writes=%0d output_writes=%0d", trial, writes, output_writes);
      for (i = 0; i < 4; i = i + 1)
        if (data[i] !== expected[i] || result[i] !== expected_result[i])
          $fatal(1, "Scalar bank mismatch trial=%0d bank=%0d data=%h expected=%h result=%h expected_result=%h",
                 trial, i, data[i], expected[i], result[i], expected_result[i]);
    end
    $display("SCALAR BANK EVENTS PASSED: 64 calls");
    $finish;
  end
endmodule
