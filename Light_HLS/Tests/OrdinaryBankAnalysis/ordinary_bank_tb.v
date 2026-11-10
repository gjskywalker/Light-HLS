`timescale 1ns/1ps

module ordinary_bank_tb;
  localparam BANK_FACTOR = `BANK_FACTOR;
  localparam ADDRESS_WIDTH = $clog2((256 + BANK_FACTOR - 1) / BANK_FACTOR);
  reg ap_clk = 0, ap_rst = 1, ap_start = 0;
  wire ap_done, ap_idle, ap_ready;
  reg [31:0] count = 0;
  wire [ADDRESS_WIDTH-1:0] input_0_address0, input_1_address0;
  wire [ADDRESS_WIDTH-1:0] input_2_address0, input_3_address0;
  wire input_0_ce0, input_1_ce0, input_2_ce0, input_3_ce0;
  wire input_0_we0, input_1_we0, input_2_we0, input_3_we0;
  wire [31:0] input_0_d0, input_1_d0, input_2_d0, input_3_d0;
  wire [31:0] input_0_q0, input_1_q0, input_2_q0, input_3_q0;
  wire [2:0] output_r_address0;
  wire output_r_ce0, output_r_we0;
  wire [31:0] output_r_d0;
  wire [ADDRESS_WIDTH-1:0] bank_address [0:3];
  wire [3:0] bank_ce = {input_3_ce0, input_2_ce0, input_1_ce0, input_0_ce0};
  wire [3:0] bank_we = {input_3_we0, input_2_we0, input_1_we0, input_0_we0};
  wire [31:0] bank_d [0:3];
  reg [31:0] response [0:3][0:`READ_LATENCY-1];
  reg [31:0] source [0:255], expected_memory [0:255];
  reg [31:0] result [0:7], expected [0:7];
  reg [31:0] random_state = 32'h932763ba, x, y;
  integer trial, index, bank, stage, address, lane, iterations;
  integer reads, writes, input_writes, cycles, expected_reads, total_results = 0;
  reg [7:0] seen;

  assign bank_address[0] = input_0_address0;
  assign bank_address[1] = input_1_address0;
  assign bank_address[2] = input_2_address0;
  assign bank_address[3] = input_3_address0;
  assign bank_d[0] = input_0_d0;
  assign bank_d[1] = input_1_d0;
  assign bank_d[2] = input_2_d0;
  assign bank_d[3] = input_3_d0;
  assign input_0_q0 = response[0][`READ_LATENCY-1];
  assign input_1_q0 = response[1][`READ_LATENCY-1];
  assign input_2_q0 = response[2][`READ_LATENCY-1];
  assign input_3_q0 = response[3][`READ_LATENCY-1];
  `DUT_NAME dut (.*);
  always #5 ap_clk = ~ap_clk;

  always @(posedge ap_clk) begin
    for (bank = 0; bank < BANK_FACTOR; bank = bank + 1) begin
      for (stage = 1; stage < `READ_LATENCY; stage = stage + 1)
        response[bank][stage] <= response[bank][stage-1];
      if (!ap_rst && bank_ce[bank] === 1'b1) begin
        if ((^bank_address[bank]) === 1'bx)
          $fatal(1, "Unknown memory request address");
        if (`CASE_KIND == 1) address = 64*bank + bank_address[bank];
        else address = BANK_FACTOR*bank_address[bank] + bank;
        if (address >= 256) $fatal(1, "Out-of-bounds memory request");
        if (bank_we[bank] === 1'b1) begin
          source[address] <= bank_d[bank];
          input_writes = input_writes + 1;
        end else begin
          response[bank][0] <= source[address];
          reads = reads + 1;
        end
      end
    end
    if (!ap_rst && output_r_ce0 === 1'b1 && output_r_we0 === 1'b1) begin
      if ((^output_r_address0) === 1'bx || output_r_address0 >= iterations)
        $fatal(1, "Invalid output request address");
      if (seen[output_r_address0]) $fatal(1, "Duplicate output write");
      seen[output_r_address0] = 1;
      result[output_r_address0] <= output_r_d0;
      writes = writes + 1;
    end
  end

  initial begin
    for (bank = 0; bank < 4; bank = bank + 1)
      for (stage = 0; stage < `READ_LATENCY; stage = stage + 1)
        response[bank][stage] = 0;
    repeat (3) @(negedge ap_clk);
    ap_rst = 0;
    for (trial = 0; trial < 64; trial = trial + 1) begin
      for (index = 0; index < 256; index = index + 1) begin
        random_state = random_state ^ (random_state << 13);
        random_state = random_state ^ (random_state >> 17);
        random_state = random_state ^ (random_state << 5);
        source[index] = random_state;
        expected_memory[index] = random_state;
      end
      count = trial;
      lane = count & 3;
      iterations = (`CASE_KIND == 4) ? (count & 7) : 8;
      for (index = 0; index < 8; index = index + 1) begin
        result[index] = 32'hdeadbeef;
        expected[index] = 32'hdeadbeef;
        if (index < iterations) begin
          case (`CASE_KIND)
            0: expected[index] = source[4*index] + source[4*index+1] +
                                 source[4*index+2] + source[4*index+3];
            1: expected[index] = source[index] + source[index+64] +
                                 source[index+128] + source[index+192];
            2: expected[index] = source[4*index] + source[4*index+128];
            3: expected[index] = source[4*index+lane];
            4: expected[index] = source[4*index] + 1;
            5: expected[index] = source[(252+4*index) & 255];
            6: expected[index] = source[(252+index) & 255];
            7: expected[index] = source[28-4*index];
            8: begin
              x = expected_memory[4*index+lane];
              expected_memory[4*index] = x + 5;
              y = expected_memory[4*index+lane];
              expected_memory[4*index+1] = y + 7;
              expected[index] = x + y;
            end
            9: expected[index] = source[4*index] + source[4*index+1];
          endcase
        end
      end
      expected_reads = iterations;
      if (`CASE_KIND == 0 || `CASE_KIND == 1) expected_reads = 4*iterations;
      if (`CASE_KIND == 2 || `CASE_KIND == 8 || `CASE_KIND == 9)
        expected_reads = 2*iterations;
      reads = 0;
      writes = 0;
      input_writes = 0;
      seen = 0;
      ap_start = 1;
      @(negedge ap_clk);
      ap_start = 0;
      // Inputs may change after the accepted start; the DUT must capture them.
      count = 32'hfedcba98;
      cycles = 0;
      while (!ap_done && cycles < 2000) begin
        @(negedge ap_clk);
        cycles = cycles + 1;
      end
      if (!ap_done || writes != iterations || reads != expected_reads)
        $fatal(1, "Request count/timeout trial=%0d reads=%0d/%0d writes=%0d/%0d",
               trial, reads, expected_reads, writes, iterations);
      if (input_writes != ((`CASE_KIND == 8) ? 16 : 0))
        $fatal(1, "Unexpected input write count: %0d", input_writes);
      for (index = 0; index < 8; index = index + 1)
        if (result[index] !== expected[index])
          $fatal(1, "Output mismatch trial=%0d index=%0d expected=%h actual=%h",
                 trial, index, expected[index], result[index]);
      for (index = 0; index < 256; index = index + 1)
        if (source[index] !== expected_memory[index])
          $fatal(1, "Memory mismatch trial=%0d index=%0d", trial, index);
      total_results = total_results + iterations;
      @(negedge ap_clk);
    end
    $display("ORDINARY BANK TESTS PASSED: calls=64 outputs=%0d", total_results);
    $finish;
  end
endmodule
