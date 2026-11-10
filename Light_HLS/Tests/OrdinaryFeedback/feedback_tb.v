`timescale 1ns/1ps

module feedback_tb;
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  wire [31:0] ap_return;
  reg [31:0] seed, count, step;
  wire [7:0] input_r_address0;
  wire input_r_ce0;
  reg [31:0] input_r_q0 = 0;
  wire [3:0] output_r_address0;
  wire output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  reg [31:0] source [0:255], result [0:15], expected [0:15];
  reg [31:0] delayed_read = 0, random_state = 32'h856aabc3, sampled_seed, sampled_step;
  reg [15:0] seen;
  integer trial, index, address, expected_count, expected_address;
  integer cycles, reads, writes;
  feedback dut (.*);
  always #5 ap_clk = ~ap_clk;

  always @(posedge ap_clk) begin
    if (!ap_rst && input_r_ce0) begin
      if ((^input_r_address0) === 1'bx) $fatal(1, "Unknown input address");
      if (`READ_LATENCY == 2) delayed_read <= source[input_r_address0];
      else input_r_q0 <= source[input_r_address0];
      reads = reads + 1;
    end
    if (`READ_LATENCY == 2) input_r_q0 <= delayed_read;
    if (!ap_rst && output_r_ce0 && output_r_we0) begin
      if ((^output_r_address0) === 1'bx || output_r_address0 >= expected_count)
        $fatal(1, "Unknown or out-of-range write address");
      if (seen[output_r_address0]) $fatal(1, "Duplicate write");
      seen[output_r_address0] = 1;
      result[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end
  end

  initial begin
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    for (trial = 0; trial < 64; trial = trial + 1) begin
      for (index = 0; index < 256; index = index + 1) begin
        random_state = random_state ^ (random_state << 13);
        random_state = random_state ^ (random_state >> 17);
        random_state = random_state ^ (random_state << 5);
        source[index] = random_state;
      end
      sampled_seed = (trial * 13 + 250) ^ (random_state & 32'hffffff00);
      sampled_step = trial % 4 == 0 ? 0 : random_state;
      expected_count = (`CASE_KIND == 2) ? trial % 16 :
                       (`CASE_KIND == 4) ? trial % 16 + 1 : 16;
      if (`CASE_KIND == 4 && trial % 2 == 0)
        sampled_step = (sampled_seed + 3*(trial % 5 + 1)) & 255;
      seed = sampled_seed;
      step = sampled_step;
      count = (`CASE_KIND == 4) ? expected_count - 1 : expected_count;
      address = sampled_seed & 255;
      for (index = 0; index < expected_count; index = index + 1) begin
        expected[index] = source[address] ^ (address << 24) ^ sampled_seed;
        result[index] = 32'hdeadbeef;
        case (`CASE_KIND)
          1: address = (address - 3) & 255;
          3: address = (address + sampled_step) & 255;
          6: address = (address + ((source[address] & 1) ? 3 : -5)) & 255;
          default: address = (address + 3) & 255;
        endcase
        if (`CASE_KIND == 4 && address == (sampled_step & 255))
          expected_count = index + 1;
      end
      expected_address = address;
      if (`CASE_KIND == 7 && (address & 128))
        expected_address = address | 32'hffffff00;
      reads = 0;
      writes = 0;
      seen = 0;
      ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0;
      seed = ~sampled_seed;
      step = ~sampled_step;
      count = 0;
      cycles = 0;
      while (!ap_done && cycles < 10000) begin
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (!ap_done || reads != expected_count || writes != expected_count ||
          ap_return !== expected_address)
        $fatal(1, "Control mismatch trial=%0d reads=%0d writes=%0d return=%0d expected=%0d",
               trial, reads, writes, ap_return, expected_address);
      for (index = 0; index < expected_count; index = index + 1)
        if (result[index] !== expected[index])
          $fatal(1, "Output mismatch trial=%0d index=%0d expected=%h actual=%h", trial, index, expected[index], result[index]);
      repeat (3) @(negedge ap_clk);
    end
    $display("ORDINARY FEEDBACK TESTS PASSED: 64 calls and returns");
    $finish;
  end
endmodule
