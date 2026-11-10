`timescale 1ns/1ps
`ifndef READ_LATENCY
`define READ_LATENCY 1
`endif
`ifndef DUT_NAME
`define DUT_NAME storage_stress
`endif

module storage_stress_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  wire [6:0] input_r_address0;
  wire [4:0] output_r_address0;
  wire input_r_ce0, output_r_ce0, output_r_we0;
  reg [31:0] input_r_q0 = 0;
  reg [31:0] input_pending = 0;
`ifdef BANKED_INPUT
  wire [4:0] input_0_address0, input_1_address0, input_2_address0, input_3_address0;
  wire input_0_ce0, input_1_ce0, input_2_ce0, input_3_ce0;
  reg [31:0] input_0_q0 = 0, input_1_q0 = 0, input_2_q0 = 0, input_3_q0 = 0;
  reg [31:0] bank_pending_0 = 0, bank_pending_1 = 0;
  reg [31:0] bank_pending_2 = 0, bank_pending_3 = 0;
`endif
`ifdef DUAL_INPUT
  wire [6:0] input_r_address1;
  wire input_r_ce1;
  reg [31:0] input_r_q1 = 0;
`endif
  wire [31:0] output_r_d0;
  reg [31:0] source [0:95], result [0:31];
  reg [31:0] expected, a, b, c, random_state = 32'h932763ba;
  integer trial, index, cycles, writes;
  `DUT_NAME dut (.*);
  always #5 ap_clk = ~ap_clk;
  always @(posedge ap_clk) begin
`ifdef BANKED_INPUT
    if (`READ_LATENCY == 1) begin
      if (input_0_ce0) input_0_q0 <= source[4 * input_0_address0];
      if (input_1_ce0) input_1_q0 <= source[4 * input_1_address0 + 1];
      if (input_2_ce0) input_2_q0 <= source[4 * input_2_address0 + 2];
      if (input_3_ce0) input_3_q0 <= source[4 * input_3_address0 + 3];
    end else begin
      if (input_0_ce0) bank_pending_0 <= source[4 * input_0_address0];
      if (input_1_ce0) bank_pending_1 <= source[4 * input_1_address0 + 1];
      if (input_2_ce0) bank_pending_2 <= source[4 * input_2_address0 + 2];
      if (input_3_ce0) bank_pending_3 <= source[4 * input_3_address0 + 3];
      input_0_q0 <= bank_pending_0;
      input_1_q0 <= bank_pending_1;
      input_2_q0 <= bank_pending_2;
      input_3_q0 <= bank_pending_3;
    end
`endif
`ifdef DUAL_INPUT
    if (input_r_ce1) input_r_q1 <= source[input_r_address1];
`endif
    if (`READ_LATENCY == 1) begin
      if (input_r_ce0) input_r_q0 <= source[input_r_address0];
    end else begin
      if (input_r_ce0) input_pending <= source[input_r_address0];
      input_r_q0 <= input_pending;
    end
    if (output_r_ce0 && output_r_we0) begin
      result[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end
  end
  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    for (trial = 0; trial < 64; trial = trial + 1) begin
      for (index = 0; index < 96; index = index + 1) begin
        random_state = random_state ^ (random_state << 13);
        random_state = random_state ^ (random_state >> 17);
        random_state = random_state ^ (random_state << 5);
        source[index] = random_state;
      end
      for (index = 0; index < 32; index = index + 1)
        result[index] = 32'hdeadbeef;
      writes = 0;
      ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0;
      cycles = 0;
      while (!ap_done && cycles < 1024) begin
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (!ap_done || writes != 32) $fatal(1, "Storage stress timeout or lost writes");
      for (index = 0; index < 32; index = index + 1) begin
        a = source[index];
`ifdef DUAL_INPUT
        b = source[index + 32];
        c = 0;
`else
        b = source[a & 31];
`ifdef TWO_READS
        c = 0;
`else
        c = source[(b & 31) + 32];
`endif
`endif
        expected = a + b + c;
        if (result[index] !== expected)
          $fatal(1, "Storage stress trial=%0d index=%0d expected=%h actual=%h",
                 trial, index, expected, result[index]);
      end
      @(negedge ap_clk);
    end
    $display("ALL PIPELINE STORAGE STRESS TESTS PASSED (2048 results)");
    $finish;
  end
endmodule
